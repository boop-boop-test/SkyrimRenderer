#include "SkyrimRenderer/ItemPreview.h"
#include "SkyrimRenderer/SceneManager.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace SkyrimRenderer::ItemPreview {

namespace {

constexpr float kDefaultFov = 50.0f;
constexpr float kMinDistance = 12.0f;   // CameraController clamps at 10
constexpr float kMargin = 1.1f;

std::string MeshPath(const char* model) {
    if (!model || !*model) return {};
    std::string path = model;
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("meshes\\", 0) == 0 || lower.rfind("meshes/", 0) == 0) return path;
    return "meshes\\" + path;
}

// A template, not TESModel*: the arrays hold TESModelTextureSwap, a larger type.
template <class Model>
const char* SexModel(const Model (&models)[RE::SEXES::kTotal], bool female) {
    const char* path = models[female ? RE::SEXES::kFemale : RE::SEXES::kMale].GetModel();
    if (!path || !*path) path = models[female ? RE::SEXES::kMale : RE::SEXES::kFemale].GetModel();
    return path;
}

std::string WornModel(RE::TESObjectARMO* armor, bool female) {
    RE::TESRace* races[2] = {nullptr, armor->race};
    if (auto* player = RE::PlayerCharacter::GetSingleton()) races[0] = player->GetRace();
    for (auto* race : races) {
        if (!race) continue;
        for (auto* addon : armor->armorAddons) {
            if (!addon || !addon->IsValidRace(race)) continue;
            auto path = MeshPath(SexModel(addon->bipedModels, female));
            if (!path.empty()) return path;
        }
    }
    for (auto* addon : armor->armorAddons) {
        if (!addon) continue;
        auto path = MeshPath(SexModel(addon->bipedModels, female));
        if (!path.empty()) return path;
    }
    return {};
}

} // namespace

std::string ResolveModelPath(RE::FormID formID, uint32_t flags) {
    auto* form = RE::TESForm::LookupByID(formID);
    if (!form) return {};
    if (auto* ref = form->AsReference()) form = ref->GetBaseObject();
    if (!form) return {};
    const bool female = (flags & kFemale) != 0;

    if (auto* armor = form->As<RE::TESObjectARMO>()) {
        const auto ground = MeshPath(SexModel(armor->worldModels, female));
        if (flags & kGroundModel) return !ground.empty() ? ground : WornModel(armor, female);
        auto worn = WornModel(armor, female);
        return !worn.empty() ? worn : ground;
    }
    if (auto* addon = form->As<RE::TESObjectARMA>()) {
        return MeshPath(SexModel(addon->bipedModels, female));
    }
    if (auto* model = form->As<RE::TESModel>()) {
        return MeshPath(model->GetModel());
    }
    return {};
}

uint32_t Load(SceneHandle sceneHandle, RE::FormID formID, uint32_t flags) {
    auto& mgr = SceneManager::GetInstance();
    auto* scene = mgr.GetScene(sceneHandle);
    if (!scene) return 0;

    const auto path = ResolveModelPath(formID, flags);
    if (path.empty()) {
        SKSE::log::error("[SkyrimRenderer] LoadItemPreview: form 0x{:08X} has no model", formID);
        return 0;
    }

    if (!(flags & kAppend)) scene->ClearLoadedMeshes();
    const uint32_t shapes = mgr.LoadMesh(sceneHandle, path);
    if (shapes == 0) {
        SKSE::log::error("[SkyrimRenderer] LoadItemPreview: form 0x{:08X}: no shapes in {}", formID, path);
        return 0;
    }
    if (!(flags & kKeepBackground)) scene->SetBackground(Color{0.0f, 0.0f, 0.0f, 0.0f});
    if (!(flags & kKeepCamera)) Frame(*scene);

    SKSE::log::info("[SkyrimRenderer] LoadItemPreview: form 0x{:08X} -> {} ({} shapes)", formID, path, shapes);
    return shapes;
}

void Frame(RenderScene& scene) {
    const auto& meshes = scene.GetLoadedMeshes();
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& m : meshes) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], m.boundsMin[a]);
            hi[a] = std::max(hi[a], m.boundsMax[a]);
        }
    }
    if (lo[0] > hi[0]) return;

    // The camera orbits the bounds' centre in X/Y (LoadNifMesh sets it);
    // the focal height is the centre's Z.
    const float c[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};

    // 3/4 view. Yaw 0 looks along -Y (the front of a worn piece); an item
    // long on Y (a weapon lying along it) is turned so its length faces the
    // camera instead of pointing at it.
    const float yawDeg = (hi[1] - lo[1]) > (hi[0] - lo[0]) * 1.2f ? 60.0f : 30.0f;
    constexpr float pitchDeg = 20.0f;
    constexpr float kDeg = 3.14159265f / 180.0f;
    const float sy = std::sin(yawDeg * kDeg), cy = std::cos(yawDeg * kDeg);
    const float sp = std::sin(pitchDeg * kDeg), cp = std::cos(pitchDeg * kDeg);

    // Camera basis (CameraController::Update): toCam from target to eye.
    const float toCam[3] = {sy * cp, cy * cp, sp};
    const float fwd[3] = {-toCam[0], -toCam[1], -toCam[2]};
    float right[3] = {fwd[1], -fwd[0], 0.0f};   // fwd x (0,0,1)
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    right[0] /= rl;
    right[1] /= rl;
    const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                         right[0] * fwd[1] - right[1] * fwd[0]};

    const float aspect = static_cast<float>(scene.GetWidth()) / static_cast<float>(std::max(1u, scene.GetHeight()));
    // Smallest distance with every bounds corner inside the frustum (+margin).
    auto fit = [&](float fovDeg) {
        const float tanV = std::tan(fovDeg * 0.5f * kDeg), tanH = tanV * aspect;
        float d = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float q[3] = {((i & 1) ? hi[0] : lo[0]) - c[0], ((i & 2) ? hi[1] : lo[1]) - c[1],
                                ((i & 4) ? hi[2] : lo[2]) - c[2]};
            const float x = q[0] * right[0] + q[1] * right[1] + q[2] * right[2];
            const float y = q[0] * up[0] + q[1] * up[1] + q[2] * up[2];
            const float z = q[0] * toCam[0] + q[1] * toCam[1] + q[2] * toCam[2];
            d = std::max({d, z + kMargin * std::abs(x) / tanH, z + kMargin * std::abs(y) / tanV});
        }
        return d;
    };

    float fov = kDefaultFov;
    float distance = fit(fov);
    while (distance < kMinDistance && fov > 10.5f) {
        fov = std::max(10.0f, fov * 0.7f);
        distance = fit(fov);
    }
    scene.SetFOV(fov);
    scene.SetCamera(yawDeg, pitchDeg, std::max(distance, kMinDistance), c[2]);
}

} // namespace SkyrimRenderer::ItemPreview
