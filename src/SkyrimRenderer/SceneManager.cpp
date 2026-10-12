#include "SkyrimRenderer/SceneManager.h"
#include "SkyrimRenderer/DX11StateGuard.h"
#include "SkyrimRenderer/CommunityShaders.h"
#include "SkyrimRenderer/HKXLoader.h"
#include "SkyrimRenderer/TextureCache.h"
#include "SkyrimRenderer/TextureLoader.h"

#include <RE/R/Renderer.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <unordered_set>


namespace SkyrimRenderer {

SceneManager& SceneManager::GetInstance() {
    static SceneManager instance;
    return instance;
}

SceneManager::~SceneManager() {
    Shutdown();
}

// ============================================================================
// Present Hook
// ============================================================================

void SceneManager::SetPerFrameCallback(PerFrameCallback callback) {
    std::lock_guard lock(mutex_);
    perFrameCallback_ = std::move(callback);
}

namespace {

constexpr int kPresentIndex = 8;      // IDXGISwapChain::Present
constexpr int kSwapChainSlots = 18;   // IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) + IDXGISwapChain(10)

bool IsExecutableAddress(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD protect = mbi.Protect & 0xFF;
    return (protect & kExec) != 0 && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

std::filesystem::path ModulePathForAddress(const void* p) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(p), &mod) || !mod) {
        return {};
    }
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    return n ? std::filesystem::path(buf) : std::filesystem::path{};
}

std::filesystem::path LoadedModulePath(const wchar_t* baseName) {
    HMODULE mod = GetModuleHandleW(baseName);
    if (!mod) return {};
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    return n ? std::filesystem::path(buf) : std::filesystem::path{};
}

bool IsUnderSystemDirectory(const std::filesystem::path& p) {
    wchar_t sys[MAX_PATH]{};
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    std::error_code ec;
    auto rel = std::filesystem::relative(p, sys, ec);
    if (ec || rel.empty()) return false;
    return rel.native().rfind(L"..", 0) != 0;
}

} // namespace

HRESULT STDMETHODCALLTYPE SceneManager::HookedPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    auto& inst = GetInstance();

    // Only the engine's own swapchain gets our work. The identity test is a
    // pointer compare, never swapChain->GetDevice(): a wrapper that hands its
    // device out without adding a reference costs the game one reference per
    // frame until the device is freed (Magelight 0.30.3).
    if (static_cast<void*>(swapChain) != gameSwapChain_ || inst.IsDeviceLost()) {
        return originalPresent_(swapChain, syncInterval, flags);
    }

    static uint32_t presentDiagCount = 0;
    if (++presentDiagCount % 300 == 1) {
        SKSE::log::info("[SkyrimRenderer] HookedPresent heartbeat: perFrameCallbackSet={}",
                        static_cast<bool>(inst.perFrameCallback_));
    }

    // 0. Per-frame callback (e.g., flush deferred renders)
    if (inst.perFrameCallback_) {
        inst.perFrameCallback_();
    }

    // 1. Animations + offscreen renders / readbacks
    inst.Tick();

    // 2. Composite BEFORE Present into the engine's framebuffer. Other hooks
    //    earlier in the Present chain may have already drawn; our quads draw
    //    over that output, then Present shows the final frame.
    inst.CompositeAllScenes();

    // 3. Present
    return originalPresent_(swapChain, syncInterval, flags);
}

void SceneManager::QueueTickTask(std::function<void()> task) {
    std::lock_guard lock(tickTasksMutex_);
    tickTasks_.push_back(std::move(task));
}

void SceneManager::Tick() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(tickTasksMutex_);
        tasks.swap(tickTasks_);
    }
    for (auto& task : tasks) task();

    if (IsDeviceLost()) return;

    // Pick up Community Shaders' live buffers/textures from CS's own bindings,
    // before any of our renders touch the pixel stage this frame.
    if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
        CommunityShaders::LiveResources::Get().Update(
            reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context));
    }

    // Advance playing animations (re-skins meshes, queues re-renders)
    auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (lastFrameTime_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - lastFrameTime_).count();
        dt = std::min(dt, 0.1f);  // cap at 100ms to avoid huge jumps on hitches
    }
    lastFrameTime_ = now;
    if (dt > 0.0f) {
        TickAnimations(dt);
    }

    ProcessPendingRenders();
}

void SceneManager::LogPresentLayers(const void* presentSlot) {
    auto owner = ModulePathForAddress(presentSlot);
    SKSE::log::info("[SkyrimRenderer] Present slot {} is in {}", presentSlot,
                    owner.empty() ? std::string("<unknown module>") : owner.filename().string());

    struct Layer { const wchar_t* module; const char* what; bool dropsPresent; };
    static constexpr Layer kLayers[] = {
        { L"sl.interposer.dll",   "NVIDIA Streamline (Community Shaders / Open Shaders DLSS, frame generation)", true },
        { L"nvngx_dlssg.dll",     "DLSS frame generation",                                                      true },
        { L"NvPresent64.dll",     "NVIDIA Smooth Motion (driver frame generation)",                             true },
        { L"SkyrimUpscaler.dll",  "Skyrim Upscaler (HUD Fix drops draws at Present)",                           true },
        { L"CommunityShaders.dll","Community Shaders",                                                           false },
        { L"ReShade64.dll",       "ReShade",                                                                     false },
    };
    bool frameGen = false;
    for (const auto& layer : kLayers) {
        auto path = LoadedModulePath(layer.module);
        if (path.empty()) continue;
        SKSE::log::info("[SkyrimRenderer] Present layer: {} ({})", path.filename().string(), layer.what);
        frameGen |= layer.dropsPresent;
    }

    // ENB and ReShade load as a d3d11.dll / dxgi.dll beside the game executable.
    for (const wchar_t* proxy : { L"d3d11.dll", L"dxgi.dll" }) {
        auto path = LoadedModulePath(proxy);
        if (!path.empty() && !IsUnderSystemDirectory(path)) {
            SKSE::log::info("[SkyrimRenderer] Present layer: proxy {} (ENB / ReShade style) at {}",
                            path.filename().string(), path.string());
        }
    }

    // Community Shaders' frame-generation swapchain owns the Present slot itself.
    if (!owner.empty()) {
        auto name = owner.filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "communityshaders.dll") {
            SKSE::log::info("[SkyrimRenderer] Present layer: Community Shaders frame-generation swapchain proxy");
            frameGen = true;
        } else if (name != "dxgi.dll") {
            SKSE::log::info("[SkyrimRenderer] Present slot already belongs to another layer ({})", name);
        }
    }

    if (frameGen) {
        SKSE::log::warn("[SkyrimRenderer] A frame-generation / HUD-fix layer is loaded: anything drawn at "
                        "Present (composite rects, overlays) may be dropped. Texture-mode consumers are unaffected.");
    }
}

bool SceneManager::InstallPresentHook() {
    if (hookInstalled_) return true;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) {
        SKSE::log::error("[SkyrimRenderer] BSGraphics::Renderer not available for Present hook");
        return false;
    }

    auto& rtData = renderer->GetRuntimeData();
    auto* swapChain = rtData.renderWindows[0].swapChain;
    if (!swapChain) {
        SKSE::log::error("[SkyrimRenderer] SwapChain not available for Present hook");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!vtable) {
        SKSE::log::error("[SkyrimRenderer] SwapChain has no vtable; Present hook not installed");
        return false;
    }

    // Every IDXGISwapChain slot must point into committed executable memory
    // before anything is written. A proxy with a short or odd vtable (Community
    // Shaders' frame-generation swapchain) is left alone: writing into such a
    // table broke GetDevice and other plugins' interfaces (Magelight 0.30.5).
    for (int i = 0; i < kSwapChainSlots; ++i) {
        if (!IsExecutableAddress(vtable[i])) {
            SKSE::log::error("[SkyrimRenderer] SwapChain vtable slot {} ({}) is not code; swapchain "
                             "untrusted, Present hook not installed", i, vtable[i]);
            return false;
        }
    }

    LogPresentLayers(vtable[kPresentIndex]);

    swapChainVtable_ = vtable;
    originalPresent_ = reinterpret_cast<PresentFn>(swapChainVtable_[kPresentIndex]);
    gameSwapChain_ = swapChain;

    // Make vtable writable, patch, restore protection
    DWORD oldProtect;
    if (!VirtualProtect(&swapChainVtable_[kPresentIndex], sizeof(void*),
                        PAGE_READWRITE, &oldProtect)) {
        SKSE::log::error("[SkyrimRenderer] VirtualProtect failed for Present hook");
        swapChainVtable_ = nullptr;
        originalPresent_ = nullptr;
        gameSwapChain_ = nullptr;
        return false;
    }

    swapChainVtable_[kPresentIndex] = reinterpret_cast<void*>(&HookedPresent);
    VirtualProtect(&swapChainVtable_[kPresentIndex], sizeof(void*), oldProtect, &oldProtect);

    hookInstalled_ = true;
    SKSE::log::info("[SkyrimRenderer] Present hook installed (swapchain={} original={})",
                    gameSwapChain_, (void*)originalPresent_);
    return true;
}

void SceneManager::UninstallPresentHook() {
    if (!hookInstalled_ || !swapChainVtable_ || !originalPresent_) return;

    DWORD oldProtect;
    if (VirtualProtect(&swapChainVtable_[kPresentIndex], sizeof(void*),
                       PAGE_READWRITE, &oldProtect)) {
        swapChainVtable_[kPresentIndex] = reinterpret_cast<void*>(originalPresent_);
        VirtualProtect(&swapChainVtable_[kPresentIndex], sizeof(void*),
                       oldProtect, &oldProtect);
    }

    hookInstalled_ = false;
    gameSwapChain_ = nullptr;
    SKSE::log::info("[SkyrimRenderer] Present hook uninstalled");
}

// ============================================================================
// Device health
// ============================================================================

bool SceneManager::CheckDeviceHealth(const char* where) {
    if (IsDeviceLost()) return false;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return true;
    auto* device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
    if (!device) return true;

    HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return true;

    deviceLostReason_ = static_cast<uint32_t>(reason);
    deviceLost_.store(true, std::memory_order_release);
    SKSE::log::error("[SkyrimRenderer] D3D11 device removed (reason 0x{:08X}) after {}; "
                     "rendering is off for the rest of the session",
                     static_cast<uint32_t>(reason), where);
    return false;
}

Result SceneManager::GuardedRender(RenderScene* scene, const char* where) {
    if (!scene) return Result::InvalidHandle;
    if (IsDeviceLost()) return Result::DeviceLost;

    // Head previews: upload the caller-written vertex arrays that changed.
    std::vector<ActorPreview*> heads;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, actor] : actors_) {
            if (actor && actor->IsHeadOnly() && &actor->GetScene() == scene) heads.push_back(actor.get());
        }
    }
    for (auto* head : heads) head->FlushDrivenHead();

    Result result = scene->Render();
    if (!CheckDeviceHealth(where)) return Result::DeviceLost;
    return result;
}

void SceneManager::RequestRender(SceneHandle scene) {
    std::lock_guard lock(mutex_);
    pendingRenders_.push_back(scene);
}

void SceneManager::SetRenderCompleteCallback(RenderCompleteCallback callback) {
    std::lock_guard lock(mutex_);
    renderCompleteCallback_ = std::move(callback);
}

void SceneManager::ProcessPendingRenders() {
    // Two-phase async pipeline:
    //   Phase 1: Try to collect PREVIOUS frame's readback (non-blocking)
    //   Phase 2: Render all pending composite / texture scenes + at most one readback scene
    //
    // Scenes with a composite rect are drawn directly to the backbuffer (zero CPU cost).
    // Texture-mode scenes render into their texture and stop there.
    // Any other scene uses async readback → callback (for export/fallback).
    //
    // The callback runs right here on the calling thread (Present hook or the
    // consumer's main-thread Tick). Nothing in this path posts SKSE tasks: on VR
    // a post from inside Present can deadlock the game (Magelight 0.30.1).

    if (IsDeviceLost()) return;

    std::vector<SceneHandle> gpuOnlyHandles;
    SceneHandle readbackHandle = SceneHandle::Invalid;
    RenderCompleteCallback callback;
    {
        std::lock_guard lock(mutex_);

        // Deduplicate by handle (keep last occurrence of each)
        std::unordered_set<uint32_t> seen;
        for (auto it = pendingRenders_.rbegin(); it != pendingRenders_.rend(); ++it) {
            auto id = static_cast<uint32_t>(*it);
            if (!seen.insert(id).second) continue;

            auto sceneIt = scenes_.find(id);
            bool textureMode = sceneIt != scenes_.end() && sceneIt->second->IsTextureMode();
            if (textureMode || compositeRects_.contains(id)) {
                gpuOnlyHandles.push_back(*it);
            } else {
                // Only the last readback scene wins (single readback slot)
                if (!IsValid(readbackHandle)) {
                    readbackHandle = *it;
                }
            }
        }
        pendingRenders_.clear();

        callback = renderCompleteCallback_;
    }

    // Phase 1: Try to collect previous frame's readback (non-blocking)
    if (readbackPending_) {
        auto* prevScene = GetScene(readbackScene_);
        if (prevScene) {
            uint32_t w, h;
            auto result = prevScene->TryCompleteReadback(pixelBuffer_, w, h);
            if (Succeeded(result)) {
                readbackPending_ = false;
                if (callback) {
                    callback(readbackScene_, pixelBuffer_, w, h);
                }
            } else if (Failed(result)) {
                readbackPending_ = false;
            }
        } else {
            readbackPending_ = false;
        }
    }

    // Phase 2a: Render all composite / texture scenes (GPU-only, no readback needed)
    for (auto h : gpuOnlyHandles) {
        auto* scene = GetScene(h);
        if (scene) {
            auto result = GuardedRender(scene, "pending render");
            if (result == Result::DeviceLost) return;
            if (Failed(result)) {
                SKSE::log::error("[SkyrimRenderer] Frame render failed for scene {}",
                                static_cast<uint32_t>(h));
            }
        }
    }

    // Phase 2b: Render the readback scene (at most one per frame)
    if (IsValid(readbackHandle)) {
        auto* scene = GetScene(readbackHandle);
        if (scene) {
            auto result = GuardedRender(scene, "pending readback render");
            if (Succeeded(result)) {
                scene->BeginReadback();
                readbackPending_ = true;
                readbackScene_ = readbackHandle;
            } else if (result != Result::DeviceLost) {
                SKSE::log::error("[SkyrimRenderer] Frame render failed for scene {}",
                                static_cast<uint32_t>(readbackHandle));
            }
        }
    }
}

// ============================================================================
// Configuration
// ============================================================================

void SceneManager::SetShaderDirectory(const std::filesystem::path& dir) {
    std::lock_guard lock(mutex_);
    shaderDir_ = dir;
    SKSE::log::info("[SkyrimRenderer] Shader directory: {}", dir.string());
}

void SceneManager::PrecompileShaders() {
    std::lock_guard lock(mutex_);

    if (shaderDir_.empty()) {
        SKSE::log::warn("[SkyrimRenderer] PrecompileShaders: shader directory not set");
        return;
    }

    // Get the D3D11 device from Skyrim's renderer
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) {
        SKSE::log::warn("[SkyrimRenderer] PrecompileShaders: renderer not available");
        return;
    }

    auto& rendererData = renderer->GetRuntimeData();
    auto* device = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
    if (!device) {
        SKSE::log::warn("[SkyrimRenderer] PrecompileShaders: D3D11 device not available");
        return;
    }

    // Create a temporary ShaderPipeline just to warm the static blob cache.
    // The compiled blobs persist in ShaderPipeline::blobCache_ across all instances.
    ShaderPipeline tempPipeline;
    tempPipeline.Initialize(device, shaderDir_);
    auto result = tempPipeline.Compile();
    if (Failed(result)) {
        SKSE::log::error("[SkyrimRenderer] PrecompileShaders: compilation failed");
    } else {
        SKSE::log::info("[SkyrimRenderer] PrecompileShaders: shader blobs cached");
    }
    tempPipeline.Release();
}

// ============================================================================
// Scene API
// ============================================================================

SceneHandle SceneManager::CreateScene(uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);

    auto scene = std::make_unique<RenderScene>(width, height, shaderDir_);
    if (!scene->GetSceneRoot()) {
        SKSE::log::error("[SkyrimRenderer] Failed to create scene ({}x{})", width, height);
        return SceneHandle::Invalid;
    }

    uint32_t id = nextSceneHandle_++;
    scenes_[id] = std::move(scene);

    SKSE::log::info("[SkyrimRenderer] Created scene {} ({}x{})", id, width, height);
    return static_cast<SceneHandle>(id);
}

RenderScene* SceneManager::GetScene(SceneHandle handle) {
    std::lock_guard lock(mutex_);
    auto it = scenes_.find(static_cast<uint32_t>(handle));
    return (it != scenes_.end()) ? it->second.get() : nullptr;
}

NodeHandle SceneManager::AddNode(SceneHandle scene, RE::NiNode* node) {
    auto* s = GetScene(scene);
    return s ? s->AddNode(node) : NodeHandle::Invalid;
}

void SceneManager::RemoveNode(SceneHandle scene, NodeHandle node) {
    if (auto* s = GetScene(scene)) {
        s->RemoveNode(node);
    }
}

void SceneManager::SetCamera(SceneHandle scene, float yaw, float pitch, float distance, float focalHeight) {
    if (auto* s = GetScene(scene)) {
        s->SetCamera(yaw, pitch, distance, focalHeight);
    }
}

void SceneManager::PanCamera(SceneHandle scene, float dx, float dy) {
    if (auto* s = GetScene(scene)) {
        s->PanCamera(dx, dy);
    }
}

void SceneManager::SetLight(SceneHandle scene, uint32_t index, const LightConfig& config) {
    if (auto* s = GetScene(scene)) {
        s->SetLight(index, config);
    }
}

void SceneManager::SetLightingPreset(SceneHandle scene, LightingPreset preset) {
    if (auto* s = GetScene(scene)) {
        s->SetLightingPreset(preset);
    }
}

void SceneManager::SetBackground(SceneHandle scene, const Color& color) {
    if (auto* s = GetScene(scene)) {
        s->SetBackground(color);
    }
}

void SceneManager::SetENBMode(SceneHandle scene, ENBMode mode) {
    if (auto* s = GetScene(scene)) {
        s->SetENBMode(mode);
    }
}

void SceneManager::SetFrameOverlay(SceneHandle scene, const FrameOverlayConfig& config) {
    if (auto* s = GetScene(scene)) {
        s->SetFrameOverlay(config);
    }
}

void SceneManager::ClearFrameOverlay(SceneHandle scene) {
    if (auto* s = GetScene(scene)) {
        s->ClearFrameOverlay();
    }
}

// ── Post-Processing ─────────────────────────────────────────────────────────

PostProcessHandle SceneManager::AddPostProcess(
    SceneHandle scene, const std::string& name,
    PostProcessStage stage, const std::string& filePath, int32_t priority)
{
    if (auto* s = GetScene(scene)) {
        return s->AddPostProcess(name, stage, filePath, priority);
    }
    return PostProcessHandle::Invalid;
}

PostProcessHandle SceneManager::AddPostProcessFromSource(
    SceneHandle scene, const std::string& name,
    PostProcessStage stage, const std::string& hlslSource, int32_t priority)
{
    if (auto* s = GetScene(scene)) {
        return s->AddPostProcessFromSource(name, stage, hlslSource, priority);
    }
    return PostProcessHandle::Invalid;
}

void SceneManager::SetPostProcessParams(
    SceneHandle scene, PostProcessHandle pass, const PostProcessParams& params)
{
    if (auto* s = GetScene(scene)) {
        s->SetPostProcessParams(pass, params);
    }
}

void SceneManager::SetPostProcessEnabled(
    SceneHandle scene, PostProcessHandle pass, bool enabled)
{
    if (auto* s = GetScene(scene)) {
        s->SetPostProcessEnabled(pass, enabled);
    }
}

void SceneManager::RemovePostProcess(SceneHandle scene, PostProcessHandle pass) {
    if (auto* s = GetScene(scene)) {
        s->RemovePostProcess(pass);
    }
}

std::vector<std::string> SceneManager::GetPostProcessStack(
    SceneHandle scene, PostProcessStage stage)
{
    if (auto* s = GetScene(scene)) {
        return s->GetPostProcessStack(stage);
    }
    return {};
}

ID3D11ShaderResourceView* SceneManager::Render(SceneHandle scene) {
    auto* s = GetScene(scene);
    if (!s) return nullptr;

    Result result = GuardedRender(s, "Render");
    if (Failed(result)) {
        SKSE::log::error("[SkyrimRenderer] Render failed for scene {}", static_cast<uint32_t>(scene));
        return nullptr;
    }

    return s->GetRenderedTexture();
}

// ============================================================================
// Texture mode
// ============================================================================

void SceneManager::SetSceneTextureMode(SceneHandle scene, bool enabled) {
    if (auto* s = GetScene(scene)) {
        s->SetTextureMode(enabled);
        if (enabled) {
            std::lock_guard lock(mutex_);
            compositeRects_.erase(static_cast<uint32_t>(scene));
        }
    }
}

Result SceneManager::RenderToTexture(SceneHandle scene) {
    auto* s = GetScene(scene);
    if (!s) return Result::InvalidHandle;
    return GuardedRender(s, "RenderToTexture");
}

bool SceneManager::GetSceneTexture(SceneHandle scene, SceneTextureInfo& out) {
    out = {};
    auto* s = GetScene(scene);
    if (!s) return false;
    out.srv = s->GetRenderedTexture();
    out.texture = s->GetRenderedTexture2D();
    out.width = s->GetWidth();
    out.height = s->GetHeight();
    return out.srv != nullptr && out.texture != nullptr;
}

bool SceneManager::RefreshLiveFace(ActorHandle actor) {
    if (IsDeviceLost()) return false;
    auto* a = GetActorPreview(actor);
    return a ? a->RefreshLiveFace() : false;
}

Result SceneManager::ExportPNG(SceneHandle scene, const char* filePath) {
    auto* s = GetScene(scene);
    return s ? s->ExportPNG(filePath) : Result::InvalidHandle;
}

Result SceneManager::ExportDDS(SceneHandle scene, const char* filePath) {
    auto* s = GetScene(scene);
    return s ? s->ExportDDS(filePath) : Result::InvalidHandle;
}

Result SceneManager::Resize(SceneHandle scene, uint32_t width, uint32_t height) {
    auto* s = GetScene(scene);
    return s ? s->Resize(width, height) : Result::InvalidHandle;
}

uint32_t SceneManager::LoadMesh(SceneHandle scene, const std::string& meshPath) {
    auto* s = GetScene(scene);
    if (!s) return 0;
    return s->LoadNifMesh(meshPath, MeshTag::None, "", nullptr, -1.0f);
}

void SceneManager::ClearMeshes(SceneHandle scene) {
    auto* s = GetScene(scene);
    if (!s) return;
    s->ClearLoadedMeshes();
    s->ClearMeshCache();
}

void SceneManager::DestroyScene(SceneHandle handle) {
    std::lock_guard lock(mutex_);

    uint32_t id = static_cast<uint32_t>(handle);

    // Destroy any actor previews associated with this scene
    std::vector<uint32_t> actorsToRemove;
    for (auto& [actorId, sceneId] : actorToScene_) {
        if (sceneId == id) {
            actorsToRemove.push_back(actorId);
        }
    }
    for (uint32_t actorId : actorsToRemove) {
        actors_.erase(actorId);
        actorToScene_.erase(actorId);
    }

    // Cancel pending readback if it was for this scene
    if (readbackPending_ && readbackScene_ == handle) {
        readbackPending_ = false;
        readbackScene_ = SceneHandle::Invalid;
    }

    // Remove composite rect if any
    compositeRects_.erase(id);

    // Remove overlay→scene associations for this scene
    for (auto it = overlayToScene_.begin(); it != overlayToScene_.end(); ) {
        if (it->second == id) it = overlayToScene_.erase(it);
        else ++it;
    }

    scenes_.erase(id);
    SKSE::log::info("[SkyrimRenderer] Destroyed scene {}", id);
}

// ============================================================================
// Actor API
// ============================================================================

ActorHandle SceneManager::LoadActorPreview(SceneHandle scene, RE::FormID actorFormID,
                                             const std::string& animationPath) {
    auto* s = GetScene(scene);
    if (!s) return ActorHandle::Invalid;

    // If an animation path is provided, start loading the HKX file in parallel
    // with actor construction (which does RebuildMeshes). The animation and mesh
    // loads are independent — this hides the ~140ms animation cost behind mesh I/O.
    std::future<std::pair<AnimationClip, SkeletonDef>> animFuture;
    bool hasAnimFuture = false;

    if (!animationPath.empty()) {
        // We need the skeleton paths for the animation loader, but they're determined
        // during ActorPreview construction. Pre-resolve them here from the actor's race.
        auto* form = RE::TESForm::LookupByID(actorFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        auto* npc = actor ? actor->GetActorBase() : nullptr;
        auto* race = npc ? npc->GetRace() : nullptr;
        if (race) {
            bool isFemale = npc->IsFemale();
            auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
            auto skeletonNifPath = std::string("meshes\\") + skelModel.model.c_str();
            auto skeletonHkxPath = ResolveSkeletonHkxPath(race, isFemale, skeletonNifPath);

            if (!skeletonHkxPath.empty()) {
                std::string animPath = animationPath;
                std::string hkxPath = skeletonHkxPath;
                std::string nifPath = skeletonNifPath;
                animFuture = std::async(std::launch::async,
                    [animPath, hkxPath, nifPath]() -> std::pair<AnimationClip, SkeletonDef> {
                        AnimationClip clip;
                        SkeletonDef skel;
                        HKXLoader::LoadAnimation(animPath, hkxPath, nifPath, clip, skel);
                        return {std::move(clip), std::move(skel)};
                    });
                hasAnimFuture = true;
            }
        }
    }

    auto preview = std::make_unique<ActorPreview>(*s, actorFormID);
    if (!preview->IsValid()) {
        SKSE::log::error("[SkyrimRenderer] Failed to create actor preview for 0x{:X}", actorFormID);
        return ActorHandle::Invalid;
    }

    // Collect the async animation result and apply it
    if (hasAnimFuture) {
        try {
            auto [clip, skel] = animFuture.get();
            if (!clip.keyframes.empty()) {
                preview->ApplyPreloadedAnimation(
                    animationPath.c_str(),
                    std::make_shared<AnimationClip>(std::move(clip)),
                    std::make_shared<SkeletonDef>(std::move(skel)));
                SKSE::log::info("[SkyrimRenderer] Applied parallel-loaded animation: {}", animationPath);
            }
        } catch (const std::exception& e) {
            SKSE::log::warn("[SkyrimRenderer] Parallel animation load failed: {}", e.what());
        }
    }

    std::lock_guard lock(mutex_);
    uint32_t id = nextActorHandle_++;
    uint32_t sceneId = static_cast<uint32_t>(scene);

    actors_[id] = std::move(preview);
    actorToScene_[id] = sceneId;

    SKSE::log::info("[SkyrimRenderer] Created actor preview {} for 0x{:X} in scene {}",
                    id, actorFormID, sceneId);
    return static_cast<ActorHandle>(id);
}

ActorHandle SceneManager::LoadActorHead(SceneHandle scene, RE::FormID formID) {
    auto* s = GetScene(scene);
    if (!s) return ActorHandle::Invalid;

    auto preview = std::make_unique<ActorPreview>(*s, formID, /*headOnly=*/true);
    if (!preview->IsValid()) {
        SKSE::log::error("[SkyrimRenderer] Failed to create head preview for 0x{:X}", formID);
        return ActorHandle::Invalid;
    }

    std::lock_guard lock(mutex_);
    uint32_t id = nextActorHandle_++;
    uint32_t sceneId = static_cast<uint32_t>(scene);
    actors_[id] = std::move(preview);
    actorToScene_[id] = sceneId;
    SKSE::log::info("[SkyrimRenderer] Created head preview {} for 0x{:X} in scene {}", id, formID, sceneId);
    return static_cast<ActorHandle>(id);
}

ActorPreview* SceneManager::GetActorPreview(ActorHandle handle) {
    std::lock_guard lock(mutex_);
    auto it = actors_.find(static_cast<uint32_t>(handle));
    return (it != actors_.end()) ? it->second.get() : nullptr;
}

Result SceneManager::EquipItem(ActorHandle actor, RE::FormID armorFormID) {
    auto* a = GetActorPreview(actor);
    return a ? a->EquipItem(armorFormID) : Result::InvalidHandle;
}

Result SceneManager::UnequipItem(ActorHandle actor, RE::FormID armorFormID) {
    auto* a = GetActorPreview(actor);
    return a ? a->UnequipItem(armorFormID) : Result::InvalidHandle;
}

Result SceneManager::EquipOutfit(ActorHandle actor, const std::vector<RE::FormID>& armorFormIDs, bool replaceAll) {
    auto* a = GetActorPreview(actor);
    return a ? a->EquipOutfit(armorFormIDs, replaceAll) : Result::InvalidHandle;
}

Result SceneManager::SetPose(ActorHandle actor, const char* animationPath) {
    auto* a = GetActorPreview(actor);
    return a ? a->SetPose(animationPath) : Result::InvalidHandle;
}

// ── Animation Playback ──────────────────────────────────────────────────────

Result SceneManager::LoadAnimation(ActorHandle actor, const char* animationPath) {
    auto* a = GetActorPreview(actor);
    return a ? a->LoadAnimation(animationPath) : Result::InvalidHandle;
}

void SceneManager::PlayAnimation(ActorHandle actor) {
    auto* a = GetActorPreview(actor);
    if (a) a->PlayAnimation();
}

void SceneManager::PauseAnimation(ActorHandle actor) {
    auto* a = GetActorPreview(actor);
    if (a) a->PauseAnimation();
}

void SceneManager::StopAnimation(ActorHandle actor) {
    auto* a = GetActorPreview(actor);
    if (a) a->StopAnimation();
}

void SceneManager::SeekAnimation(ActorHandle actor, float timeSeconds) {
    auto* a = GetActorPreview(actor);
    if (a) a->SeekAnimation(timeSeconds);
}

void SceneManager::SetAnimationSpeed(ActorHandle actor, float speed) {
    auto* a = GetActorPreview(actor);
    if (a) a->SetAnimationSpeed(speed);
}

void SceneManager::SetAnimationLooping(ActorHandle actor, bool loop) {
    auto* a = GetActorPreview(actor);
    if (a) a->SetAnimationLooping(loop);
}

void SceneManager::SetSkipBindCorrection(ActorHandle actor, bool skip) {
    auto* a = GetActorPreview(actor);
    if (a) a->SetSkipBindCorrection(skip);
}

void SceneManager::SetUseRefPoseLocals(ActorHandle actor, bool use) {
    auto* a = GetActorPreview(actor);
    if (a) a->SetUseRefPoseLocals(use);
}

void SceneManager::ApplyNifBindPose(ActorHandle actor) {
    auto* a = GetActorPreview(actor);
    if (a) a->ApplyNifBindPose();
}

ActorPreview::AnimationState SceneManager::GetAnimationState(ActorHandle actor) {
    auto* a = GetActorPreview(actor);
    return a ? a->GetAnimationState() : ActorPreview::AnimationState{};
}

void SceneManager::TickAnimations(float deltaSeconds) {
    std::lock_guard lock(mutex_);
    for (auto& [id, actor] : actors_) {
        if (actor && actor->TickAnimation(deltaSeconds)) {
            // Animation changed the pose — request a re-render for this actor's scene.
            auto sceneIt = actorToScene_.find(id);
            if (sceneIt != actorToScene_.end()) {
                auto handle = static_cast<SceneHandle>(sceneIt->second);
                // Queue a render if not already pending.
                if (std::find(pendingRenders_.begin(), pendingRenders_.end(), handle) == pendingRenders_.end()) {
                    pendingRenders_.push_back(handle);
                }
            }
        }
    }
}

void SceneManager::SetCameraPreset(SceneHandle scene, CameraPreset preset) {
    if (auto* s = GetScene(scene)) {
        s->SetCameraPreset(preset);
    }
}

void SceneManager::SetActorMorphs(ActorHandle actor, ActorMorphMap morphs) {
    std::lock_guard lock(mutex_);
    auto it = actors_.find(static_cast<uint32_t>(actor));
    if (it != actors_.end() && it->second) {
        it->second->SetMorphs(std::move(morphs));
    }
}

void SceneManager::ClearActorMorphs(ActorHandle actor) {
    std::lock_guard lock(mutex_);
    auto it = actors_.find(static_cast<uint32_t>(actor));
    if (it != actors_.end() && it->second) {
        it->second->ClearMorphs();
    }
}

void SceneManager::DestroyActorPreview(ActorHandle handle) {
    std::lock_guard lock(mutex_);

    uint32_t id = static_cast<uint32_t>(handle);
    actors_.erase(id);
    actorToScene_.erase(id);

    SKSE::log::info("[SkyrimRenderer] Destroyed actor preview {}", id);
}

// ============================================================================
// Backbuffer Compositing
// ============================================================================

void SceneManager::CompositeAllScenes() {
    if (IsDeviceLost()) return;

    // Snapshot ALL composite data under a single lock — scene jobs AND overlay
    // draws together. This prevents a race where the composite rect is removed
    // between the scene draw and the overlay filter check.
    struct CompositeJob {
        RenderScene* scene;
        ScreenRect rect;
    };
    struct OverlayDraw {
        ID3D11ShaderResourceView* srv;
        ScreenRect rect;
    };
    std::vector<CompositeJob> jobs;
    std::vector<OverlayDraw> overlayDraws;

    static uint32_t compositeBailCount = 0;

    {
        std::lock_guard lock(mutex_);
        if (compositeRects_.empty() && overlays_.empty()) {
            if (++compositeBailCount % 300 == 1) {
                SKSE::log::info("[SkyrimRenderer] CompositeAllScenes: no composite rects or overlays");
            }
            return;
        }

        for (auto& [sceneId, rect] : compositeRects_) {
            auto it = scenes_.find(sceneId);
            if (it == scenes_.end()) continue;

            auto* scene = it->second.get();
            if (scene->IsTextureMode()) continue;
            if (scene->GetRenderedTexture()) {
                jobs.push_back({scene, rect});
            }
        }

        // Snapshot overlay draws under the SAME lock as scene jobs
        static uint32_t overlayDiagCount = 0;
        bool logOverlayDiag = (++overlayDiagCount % 300 == 1);
        for (auto& [id, entry] : overlays_) {
            if (logOverlayDiag) {
                auto sceneIt = overlayToScene_.find(id);
                uint32_t attachedScene = (sceneIt != overlayToScene_.end()) ? sceneIt->second : 0;
                bool sceneHasRect = (attachedScene > 0) && (compositeRects_.find(attachedScene) != compositeRects_.end());
                SKSE::log::info(
                    "SkyrimRenderer::Overlay id={} visible={} hasTexture={} attachedScene={} sceneHasRect={} rect=({:.0f},{:.0f},{:.0f},{:.0f})",
                    id, entry.visible, (bool)entry.texture, attachedScene, sceneHasRect,
                    entry.rect.x, entry.rect.y, entry.rect.width, entry.rect.height);
            }

            if (!entry.visible || !entry.texture) continue;

            // If overlay is attached to a scene, only draw if that scene is compositing
            auto sceneIt = overlayToScene_.find(id);
            if (sceneIt != overlayToScene_.end()) {
                if (compositeRects_.find(sceneIt->second) == compositeRects_.end())
                    continue;
            }

            overlayDraws.push_back({entry.texture.Get(), entry.rect});
        }
    }

    if (jobs.empty() && overlayDraws.empty()) {
        if (++compositeBailCount % 300 == 1) {
            SKSE::log::info("[SkyrimRenderer] CompositeAllScenes: snapshot empty (rects existed but no rendered textures)");
        }
        return;
    }

    // Get device + context
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) {
        SKSE::log::error("[SkyrimRenderer] CompositeAllScenes: BSGraphics::Renderer not available");
        return;
    }
    auto& rendererData = renderer->GetRuntimeData();
    auto* device = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rendererData.context);
    if (!device || !ctx) {
        SKSE::log::error("[SkyrimRenderer] CompositeAllScenes: D3D11 device={} context={}", (void*)device, (void*)ctx);
        return;
    }

    if (!quadRenderer_.IsInitialized()) {
        if (Failed(quadRenderer_.Initialize(device))) {
            SKSE::log::error("[SkyrimRenderer] CompositeAllScenes: ScreenQuadRenderer init failed");
            return;
        }
    }

    // Draw into the engine's kFRAMEBUFFER render target, never swapChain->GetBuffer(0).
    // The engine owns that view, so there is no back-buffer reference to release
    // (an over-release freed the game's buffer on Community Shaders 1.6-1.8.3), and
    // a FLIP_DISCARD / proxied swapchain cannot hand back a stale buffer.
    auto& framebufferRT = rendererData.renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
    if (!framebufferRT.RTV) {
        static uint32_t missingCount = 0;
        if (++missingCount % 300 == 1) {
            SKSE::log::error("[SkyrimRenderer] CompositeAllScenes: kFRAMEBUFFER has no RTV (texture={})",
                             (void*)framebufferRT.texture);
        }
        return;
    }
    ID3D11RenderTargetView* backbufferRTV = framebufferRT.RTV;

    // The texture field is only needed for the size. With Community Shaders'
    // frame generation it is null while the RTV is valid, so take the size
    // from the RTV's own resource.
    D3D11_TEXTURE2D_DESC bbDesc{};
    if (framebufferRT.texture) {
        framebufferRT.texture->GetDesc(&bbDesc);
    } else {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        backbufferRTV->GetResource(resource.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) {
            static uint32_t noTextureCount = 0;
            if (++noTextureCount % 300 == 1) {
                SKSE::log::error("[SkyrimRenderer] CompositeAllScenes: kFRAMEBUFFER RTV is not a 2D texture");
            }
            return;
        }
        texture->GetDesc(&bbDesc);
        static bool loggedFallback = false;
        if (!loggedFallback) {
            loggedFallback = true;
            SKSE::log::info("[SkyrimRenderer] CompositeAllScenes: kFRAMEBUFFER texture is null (frame generation?); "
                            "using the RTV's texture ({}x{})", bbDesc.Width, bbDesc.Height);
        }
    }

    // Single state guard for the entire compositing pass
    DX11StateGuard stateGuard(ctx);

    // 1. Scene quads first (background)
    for (auto& job : jobs) {
        CompositeToBackbuffer(ctx, backbufferRTV, bbDesc.Width, bbDesc.Height,
                              job.scene, job.rect);
    }

    // 2. Overlays on top of scene quads
    static uint32_t compositeFrameCount = 0;
    if (++compositeFrameCount % 300 == 1) {
        SKSE::log::info(
            "SkyrimRenderer::Composite sceneQuads={} overlays={} fb={}x{} (kFRAMEBUFFER)",
            jobs.size(), overlayDraws.size(), bbDesc.Width, bbDesc.Height);
        for (auto& draw : overlayDraws) {
            SKSE::log::info(
                "SkyrimRenderer::Composite overlayRect=({:.0f},{:.0f},{:.0f},{:.0f})",
                draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
        }
    }

    for (auto& draw : overlayDraws) {
        quadRenderer_.Draw(ctx, backbufferRTV, draw.srv, draw.rect,
                           bbDesc.Width, bbDesc.Height, true /* premultiplied */);
    }

    CheckDeviceHealth("composite");
}

void SceneManager::CompositeToBackbuffer(ID3D11DeviceContext* ctx,
                                          ID3D11RenderTargetView* backbufferRTV,
                                          uint32_t bbWidth, uint32_t bbHeight,
                                          RenderScene* scene, const ScreenRect& rect) {
    auto* srv = scene->GetRenderedTexture();
    if (!srv) return;

    quadRenderer_.Draw(ctx, backbufferRTV, srv, rect, bbWidth, bbHeight);
}

// CompositeOverlays is now inlined into CompositeAllScenes (single-lock snapshot).

void SceneManager::SetCompositeRect(SceneHandle scene, const ScreenRect& rect) {
    std::lock_guard lock(mutex_);
    compositeRects_[static_cast<uint32_t>(scene)] = rect;
}

void SceneManager::ClearCompositeRect(SceneHandle scene) {
    std::lock_guard lock(mutex_);
    compositeRects_.erase(static_cast<uint32_t>(scene));
}

// ============================================================================
// Screen-Space Overlays
// ============================================================================

OverlayHandle SceneManager::AddOverlay(const wchar_t* imageFilePath, float width, float height) {
    if (!imageFilePath || width <= 0 || height <= 0) return OverlayHandle::Invalid;

    // Need the D3D11 device for texture loading
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return OverlayHandle::Invalid;

    auto& rendererData = renderer->GetRuntimeData();
    auto* device = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
    if (!device) return OverlayHandle::Invalid;

    auto srv = LoadTextureFromFile(device, imageFilePath);
    if (!srv) {
        SKSE::log::error("[SkyrimRenderer] Failed to load overlay texture");
        return OverlayHandle::Invalid;
    }

    std::lock_guard lock(mutex_);
    uint32_t id = nextOverlayHandle_++;
    overlays_[id] = OverlayEntry{
        .texture = std::move(srv),
        .rect = {0.0f, 0.0f, width, height},
        .visible = true
    };

    SKSE::log::info("[SkyrimRenderer] Added overlay {} ({}x{})", id, width, height);
    return static_cast<OverlayHandle>(id);
}

void SceneManager::SetOverlayPosition(OverlayHandle overlay, float x, float y) {
    std::lock_guard lock(mutex_);
    auto it = overlays_.find(static_cast<uint32_t>(overlay));
    if (it != overlays_.end()) {
        it->second.rect.x = x;
        it->second.rect.y = y;
    }
}

void SceneManager::SetOverlayVisible(OverlayHandle overlay, bool visible) {
    std::lock_guard lock(mutex_);
    auto it = overlays_.find(static_cast<uint32_t>(overlay));
    if (it != overlays_.end()) {
        it->second.visible = visible;
    }
}

void SceneManager::SetOverlaySize(OverlayHandle overlay, float width, float height) {
    std::lock_guard lock(mutex_);
    auto it = overlays_.find(static_cast<uint32_t>(overlay));
    if (it != overlays_.end()) {
        it->second.rect.width = width;
        it->second.rect.height = height;
    }
}

void SceneManager::RemoveOverlay(OverlayHandle overlay) {
    std::lock_guard lock(mutex_);
    uint32_t id = static_cast<uint32_t>(overlay);
    overlays_.erase(id);
    overlayToScene_.erase(id);
}

void SceneManager::AttachOverlayToScene(OverlayHandle overlay, SceneHandle scene) {
    std::lock_guard lock(mutex_);
    overlayToScene_[static_cast<uint32_t>(overlay)] = static_cast<uint32_t>(scene);
}

void SceneManager::DetachOverlayFromScene(OverlayHandle overlay) {
    std::lock_guard lock(mutex_);
    overlayToScene_.erase(static_cast<uint32_t>(overlay));
}


// ============================================================================
// Lifecycle
// ============================================================================

void SceneManager::Shutdown() {
    std::lock_guard lock(mutex_);

    UninstallPresentHook();

    actors_.clear();
    actorToScene_.clear();
    scenes_.clear();
    pendingRenders_.clear();
    compositeRects_.clear();
    overlays_.clear();
    overlayToScene_.clear();
    quadRenderer_.Release();
    readbackPending_ = false;
    readbackScene_ = SceneHandle::Invalid;

    TextureCache::GetInstance().Clear();

    SKSE::log::info("[SkyrimRenderer] SceneManager shutdown complete");
}

} // namespace SkyrimRenderer
