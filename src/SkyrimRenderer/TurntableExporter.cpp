#include "SkyrimRenderer/TurntableExporter.h"
#include "SkyrimRenderer/ExportEncoders.h"
#include "SkyrimRenderer/RenderTargetManager.h"

#include <SKSE/SKSE.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace SkyrimRenderer {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kMaxFrameSide = 4096;              // per-frame render size cap
constexpr uint64_t kMaxOutputPixels = 1ull << 28;     // 1 GiB of RGBA in memory
constexpr uint32_t kMaxOutputSide = 32768;
constexpr uint32_t kMaxInFlight = 8;                  // staging textures per job
constexpr size_t kMaxQueuedBytes = 256ull << 20;      // read-back frames waiting for the encoder (min 2 frames)
constexpr size_t kKeepFinished = 64;                  // finished statuses kept for polling

const char* FormatName(TurntableExporter::Format f) {
    switch (f) {
        case TurntableExporter::Format::Sheet:  return "sheet";
        case TurntableExporter::Format::Frames: return "frames";
        case TurntableExporter::Format::APNG:   return "apng";
        case TurntableExporter::Format::GIF:    return "gif";
    }
    return "?";
}

/// "dir/name.png" -> "dir/name_007.png" (at least three digits).
std::filesystem::path NumberedPath(const std::filesystem::path& base, uint32_t index, uint32_t count) {
    int digits = 3;
    for (uint32_t n = count > 0 ? count - 1 : 0; n >= 1000; n /= 10) ++digits;
    wchar_t num[16];
    swprintf_s(num, L"_%0*u", digits, index);
    auto ext = base.extension();
    if (ext.empty()) ext = L".png";
    auto name = base.stem();
    name += num;
    name += ext;
    return base.parent_path() / name;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

} // namespace

// ============================================================================
// FrameSink: per-job encoder thread
// ============================================================================

/// Takes frames in order and writes them out on its own thread, so PNG / GIF
/// compression never runs on the game's render thread.
class TurntableExporter::FrameSink {
public:
    struct FrameInfo { float yaw = 0.0f; float time = 0.0f; };

    FrameSink(const Desc& desc, uint32_t width, uint32_t height, std::vector<FrameInfo> infos)
        : desc_(desc), width_(width), height_(height), infos_(std::move(infos)) {
        thread_ = std::thread([this] { Run(); });
    }

    ~FrameSink() {
        Abort();
        if (thread_.joinable()) thread_.join();
    }

    void Push(std::vector<uint8_t> rgba) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(rgba));
        }
        wake_.notify_one();
    }

    /// No more frames: finish the file.
    void Close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        wake_.notify_one();
    }

    void Abort() {
        {
            std::lock_guard lock(mutex_);
            aborted_ = true;
        }
        wake_.notify_one();
    }

    /// True once the thread is done; `result` then holds the outcome.
    bool Finished(Result& result) {
        std::lock_guard lock(mutex_);
        if (!finished_) return false;
        result = result_;
        return true;
    }

    uint32_t Written() {
        std::lock_guard lock(mutex_);
        return written_;
    }

    /// Frames handed over but not yet taken by the encoder.
    size_t Queued() {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    void Run() {
        const Result result = Encode();
        std::lock_guard lock(mutex_);
        result_ = result;
        finished_ = true;
    }

    /// Next frame, or false when closed (all frames taken) or aborted.
    bool Next(std::vector<uint8_t>& frame, bool& aborted) {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return aborted_ || closed_ || !queue_.empty(); });
        aborted = aborted_;
        if (aborted_ || queue_.empty()) return false;
        frame = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    Result Encode() {
        const uint32_t count = static_cast<uint32_t>(infos_.size());
        const uint32_t delay = desc_.frameDelayMs;
        const auto& path = desc_.outPath;

        uint32_t cols = 1, rows = 1;
        std::vector<uint8_t> atlas;
        ExportEncoders::APNGWriter apng(width_, height_, desc_.loopCount);
        ExportEncoders::GIFWriter gif;

        switch (desc_.format) {
            case Format::Sheet:
                cols = std::min(desc_.columns, count);
                rows = (count + cols - 1) / cols;
                try {
                    atlas.assign(static_cast<size_t>(cols) * width_ * rows * height_ * 4, 0);
                } catch (const std::bad_alloc&) {
                    return Result::OutOfMemory;
                }
                break;
            case Format::GIF:
                if (!gif.Begin(path, width_, height_, desc_.loopCount)) return Result::ExportFailed;
                break;
            default:
                break;
        }

        std::vector<uint8_t> frame;
        bool aborted = false;
        uint32_t index = 0;
        while (Next(frame, aborted)) {
            switch (desc_.format) {
                case Format::Sheet: {
                    const size_t atlasStride = static_cast<size_t>(cols) * width_ * 4;
                    const size_t x = (index % cols) * static_cast<size_t>(width_) * 4;
                    const size_t y = (index / cols) * static_cast<size_t>(height_);
                    for (uint32_t r = 0; r < height_; ++r) {
                        std::memcpy(&atlas[(y + r) * atlasStride + x], &frame[static_cast<size_t>(r) * width_ * 4],
                                    static_cast<size_t>(width_) * 4);
                    }
                    break;
                }
                case Format::Frames: {
                    const Result r = ExportEncoders::WritePNG(NumberedPath(path, index, count), frame.data(),
                                                              width_, height_);
                    if (Failed(r)) return r;
                    break;
                }
                case Format::APNG:
                    if (!apng.AddFrame(frame.data(), delay)) return Result::ExportFailed;
                    break;
                case Format::GIF:
                    if (!gif.AddFrame(frame.data(), delay)) return Result::ExportFailed;
                    break;
            }
            ++index;
            std::lock_guard lock(mutex_);
            ++written_;
        }
        if (aborted) return Result::ExportFailed;
        if (index != count) return Result::ExportFailed;

        switch (desc_.format) {
            case Format::Sheet: {
                const Result r = ExportEncoders::WritePNG(path, atlas.data(), cols * width_, rows * height_);
                if (Failed(r)) return r;
                return WriteSheetJson(cols, rows);
            }
            case Format::Frames:
                SKSE::log::info("[SkyrimRenderer] Exported {} frames: {}", count,
                                ExportEncoders::PathToUtf8(NumberedPath(path, 0, count)));
                return Result::Success;
            case Format::APNG:
                return apng.Finish(path);
            case Format::GIF:
                return gif.Finish();
        }
        return Result::ExportFailed;
    }

    /// Sidecar next to the sheet: frame rects plus the yaw / clip time of each.
    Result WriteSheetJson(uint32_t cols, uint32_t rows) {
        auto jsonPath = desc_.outPath;
        jsonPath.replace_extension(L".json");
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, jsonPath.c_str(), L"wb") != 0 || !fp) return Result::ExportFailed;

        const auto image = JsonEscape(ExportEncoders::PathToUtf8(desc_.outPath.filename()));
        std::string json = fmt::format(
            "{{\n  \"image\": \"{}\",\n  \"width\": {},\n  \"height\": {},\n  \"frameWidth\": {},\n"
            "  \"frameHeight\": {},\n  \"columns\": {},\n  \"rows\": {},\n  \"frameCount\": {},\n"
            "  \"frameDelayMs\": {},\n  \"stepYaw\": {},\n  \"stepTime\": {},\n  \"frames\": [\n",
            image, cols * width_, rows * height_, width_, height_, cols, rows, infos_.size(), desc_.frameDelayMs,
            (desc_.flags & kStepYaw) ? "true" : "false", (desc_.flags & kStepTime) ? "true" : "false");
        for (size_t i = 0; i < infos_.size(); ++i) {
            json += fmt::format("    {{\"index\": {}, \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}, "
                                "\"yaw\": {:.4f}, \"time\": {:.4f}}}{}\n",
                                i, (i % cols) * width_, (i / cols) * height_, width_, height_, infos_[i].yaw,
                                infos_[i].time, i + 1 < infos_.size() ? "," : "");
        }
        json += "  ]\n}\n";
        const bool ok = fwrite(json.data(), 1, json.size(), fp) == json.size();
        fclose(fp);
        if (!ok) return Result::ExportFailed;
        SKSE::log::info("[SkyrimRenderer] Exported sprite sheet: {} ({}x{} frames of {}x{})",
                        ExportEncoders::PathToUtf8(desc_.outPath), cols, rows, width_, height_);
        return Result::Success;
    }

    Desc desc_;
    uint32_t width_, height_;
    std::vector<FrameInfo> infos_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<uint8_t>> queue_;
    bool closed_ = false;
    bool aborted_ = false;
    bool finished_ = false;
    Result result_ = Result::Pending;
    uint32_t written_ = 0;
};

// ============================================================================
// Job
// ============================================================================

struct TurntableExporter::Job {
    uint32_t id = 0;
    ConsumerId owner = 0;
    Desc desc;
    State state = State::Rendering;
    Result result = Result::Success;
    bool notified = false;

    uint32_t width = 0, height = 0;
    // Camera at Start: every frame uses it (only the yaw is stepped).
    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f, focal = 0.0f;
    std::vector<FrameSink::FrameInfo> frames;

    RenderTargetManager target;
    std::vector<ComPtr<ID3D11Texture2D>> staging;   // frame f reads back through staging[f % size]
    uint32_t nextFrame = 0;     // next frame to render
    uint32_t framesRead = 0;    // frames read back (and handed to the sink)

    std::unique_ptr<FrameSink> sink;

    void ReleaseGpu() {
        staging.clear();
        target.Release();
    }
};

TurntableExporter& TurntableExporter::Get() {
    static TurntableExporter instance;
    return instance;
}

TurntableExporter::~TurntableExporter() {
    std::lock_guard lock(mutex_);
    jobs_.clear();   // each sink aborts and joins its thread
}

uint32_t TurntableExporter::Start(const Desc& in, ConsumerId owner) {
    auto& mgr = SceneManager::GetInstance();
    auto* scene = mgr.GetScene(in.scene);
    if (!scene || !scene->GetDevice()) {
        SKSE::log::error("[SkyrimRenderer] Export: invalid scene {}", static_cast<uint32_t>(in.scene));
        return 0;
    }
    if (mgr.IsDeviceLost()) return 0;
    if (in.frameCount == 0 || in.frameCount > kMaxFrames || in.outPath.empty() ||
        static_cast<uint32_t>(in.format) > static_cast<uint32_t>(Format::GIF) ||
        !(in.flags & (kStepYaw | kStepTime))) {
        SKSE::log::error("[SkyrimRenderer] Export: bad parameters (frames {}, format {}, flags {:#x}, path {})",
                         in.frameCount, static_cast<uint32_t>(in.format), in.flags, in.outPath.empty() ? "empty" : "set");
        return 0;
    }

    auto job = std::make_unique<Job>();
    job->owner = owner;
    job->desc = in;
    auto& d = job->desc;
    const uint32_t n = d.frameCount;

    job->width = d.width ? d.width : scene->GetWidth();
    job->height = d.height ? d.height : scene->GetHeight();
    if (job->width == 0 || job->height == 0 || job->width > kMaxFrameSide || job->height > kMaxFrameSide) {
        SKSE::log::error("[SkyrimRenderer] Export: frame size {}x{} out of range (1..{})", job->width, job->height,
                         kMaxFrameSide);
        return 0;
    }

    if (d.format == Format::Sheet) {
        if (d.columns == 0) d.columns = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(n))));
        d.columns = std::clamp(d.columns, 1u, n);
        const uint64_t rows = (n + d.columns - 1) / d.columns;
        const uint64_t w = uint64_t(d.columns) * job->width, h = rows * job->height;
        if (w > kMaxOutputSide || h > kMaxOutputSide || w * h > kMaxOutputPixels) {
            SKSE::log::error("[SkyrimRenderer] Export: sprite sheet {}x{} too large", w, h);
            return 0;
        }
    }

    job->yaw = scene->GetCameraYaw();
    job->pitch = scene->GetCameraPitch();
    job->distance = scene->GetCameraDistance();
    job->focal = scene->GetCameraFocalHeight();

    // Frame i of n: start + (end - start) * i / n, so the last frame stops one
    // step short of the end and the sequence loops; kInclusiveEnd divides by
    // n - 1 instead, landing on the end. A full turn (startYaw == endYaw) is
    // always exclusive.
    const bool inclusive = (d.flags & kInclusiveEnd) && n > 1;
    float yaw0 = d.startYaw, yaw1 = d.endYaw;
    bool yawInclusive = inclusive;
    if (yaw0 == yaw1) {
        yaw1 = yaw0 + 360.0f;
        yawInclusive = false;
    }
    float t0 = d.startTime, t1 = d.endTime;
    if (d.flags & kStepTime) {
        const auto anim = mgr.GetAnimationState(d.actor);
        if (anim.duration <= 0.0f) {
            SKSE::log::error("[SkyrimRenderer] Export: actor {} has no animation clip to step",
                             static_cast<uint32_t>(d.actor));
            return 0;
        }
        t0 = std::clamp(t0, 0.0f, anim.duration);
        if (t1 <= t0) t1 = anim.duration;
    }
    job->frames.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float fy = static_cast<float>(i) / static_cast<float>(yawInclusive ? n - 1 : n);
        const float ft = static_cast<float>(i) / static_cast<float>(inclusive ? n - 1 : n);
        job->frames[i].yaw = (d.flags & kStepYaw) ? yaw0 + (yaw1 - yaw0) * fy : job->yaw;
        job->frames[i].time = (d.flags & kStepTime) ? t0 + (t1 - t0) * ft : 0.0f;
    }

    if (d.frameDelayMs == 0) {
        d.frameDelayMs = (d.flags & kStepTime)
            ? std::max(20u, static_cast<uint32_t>(std::lround((t1 - t0) * 1000.0f / static_cast<float>(n))))
            : 50u;
    }
    d.framesPerTick = std::clamp(d.framesPerTick, 1u, kMaxInFlight - 1);

    // GPU: the job's own render target plus framesPerTick + 1 staging copies,
    // so readback never waits on the GPU.
    auto* device = scene->GetDevice();
    if (Failed(job->target.Initialize(device, job->width, job->height))) {
        SKSE::log::error("[SkyrimRenderer] Export: could not create a {}x{} render target", job->width, job->height);
        return 0;
    }
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = job->width;
    sd.Height = job->height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    job->staging.resize(d.framesPerTick + 1);
    for (auto& tex : job->staging) {
        if (FAILED(device->CreateTexture2D(&sd, nullptr, tex.GetAddressOf()))) {
            SKSE::log::error("[SkyrimRenderer] Export: could not create staging textures");
            return 0;
        }
    }

    job->sink = std::make_unique<FrameSink>(d, job->width, job->height, job->frames);

    std::lock_guard lock(mutex_);
    job->id = nextJob_++;
    const uint32_t id = job->id;
    SKSE::log::info("[SkyrimRenderer] Export job {}: scene {}, {} {} frames of {}x{} ({}{}) -> {}", id,
                    static_cast<uint32_t>(d.scene), n, FormatName(d.format), job->width, job->height,
                    (d.flags & kStepYaw) ? "yaw" : "", (d.flags & kStepTime) ? " time" : "",
                    ExportEncoders::PathToUtf8(d.outPath));
    jobs_[id] = std::move(job);
    EnsureTicking();
    return id;
}

void TurntableExporter::EnsureTicking() {
    if (tickQueued_) return;
    tickQueued_ = true;
    SceneManager::GetInstance().QueueTickTask([this] { Tick(); });
}

bool TurntableExporter::GetStatus(uint32_t id, Status& out) {
    out = {};
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return false;
    const auto& job = *it->second;
    out.state = job.state;
    out.result = job.result;
    out.scene = job.desc.scene;
    out.framesRendered = job.framesRead;
    out.frameCount = job.desc.frameCount;
    out.framesWritten = job.sink ? job.sink->Written() : (job.state == State::Done ? job.desc.frameCount : 0);
    return true;
}

void TurntableExporter::Cancel(uint32_t id) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    auto& job = *it->second;
    if (job.state == State::Rendering || job.state == State::Writing) {
        FinishJob(job, State::Cancelled, Result::ExportFailed);
        EnsureTicking();   // deliver the callback
    }
}

void TurntableExporter::Release(uint32_t id) {
    std::unique_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        job = std::move(it->second);
        jobs_.erase(it);
        std::erase(finishedOrder_, id);
    }
    // Destroyed outside the lock: an unfinished sink joins its thread.
}

void TurntableExporter::SetCompleteCallback(CompleteCallback callback, ConsumerId consumer) {
    std::lock_guard lock(mutex_);
    if (callback) callbacks_[consumer] = std::move(callback);
    else callbacks_.erase(consumer);
}

void TurntableExporter::Shutdown(ConsumerId consumer) {
    std::vector<std::unique_ptr<Job>> dropped;
    {
        std::lock_guard lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->second->owner == consumer) {
                std::erase(finishedOrder_, it->first);
                dropped.push_back(std::move(it->second));
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
        callbacks_.erase(consumer);
    }
    if (!dropped.empty()) {
        SKSE::log::info("[SkyrimRenderer] Export: dropped {} job(s) of consumer {:#x}", dropped.size(), consumer);
    }
}

void TurntableExporter::FinishJob(Job& job, State state, Result result) {
    job.state = state;
    job.result = result;
    job.ReleaseGpu();
    // The sink stays for its written count; an aborted thread is joined when
    // the job is released, nothing waits on it here.
    if (job.sink && state != State::Done) job.sink->Abort();
    if (state == State::Failed) {
        SKSE::log::error("[SkyrimRenderer] Export job {} failed (result {})", job.id, static_cast<uint32_t>(result));
    } else if (state == State::Cancelled) {
        SKSE::log::info("[SkyrimRenderer] Export job {} cancelled", job.id);
    }
}

// ============================================================================
// Tick (render thread)
// ============================================================================

void TurntableExporter::TickJob(Job& job) {
    auto& mgr = SceneManager::GetInstance();
    const auto& d = job.desc;
    if (mgr.IsDeviceLost()) {
        FinishJob(job, State::Failed, Result::DeviceLost);
        return;
    }
    auto* scene = mgr.GetScene(d.scene);
    if (!scene || ((d.flags & kStepTime) && !mgr.GetActorPreview(d.actor))) {
        FinishJob(job, State::Failed, Result::InvalidHandle);
        return;
    }
    auto* ctx = scene->GetDeviceContext();
    const uint32_t n = d.frameCount;
    const auto slots = static_cast<uint32_t>(job.staging.size());

    // 1. Read back finished copies, in frame order, without waiting. The
    //    encoder is slower than the GPU (PNG / GIF compression), so stop while
    //    its queue is full: the unread copies keep their staging slots, which
    //    also holds back step 2 until the encoder drains.
    const size_t frameBytes = static_cast<size_t>(job.width) * job.height * 4;
    const size_t maxQueued = std::max<size_t>(2, kMaxQueuedBytes / std::max<size_t>(frameBytes, 1));
    while (job.framesRead < job.nextFrame && job.sink->Queued() < maxQueued) {
        auto* tex = job.staging[job.framesRead % slots].Get();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = ctx->Map(tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        if (FAILED(hr)) {
            SKSE::log::error("[SkyrimRenderer] Export job {}: Map failed ({:#x})", job.id, static_cast<uint32_t>(hr));
            FinishJob(job, State::Failed, Result::RenderFailed);
            return;
        }
        const size_t row = static_cast<size_t>(job.width) * 4;
        std::vector<uint8_t> pixels(row * job.height);
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t r = 0; r < job.height; ++r) std::memcpy(&pixels[r * row], src + r * mapped.RowPitch, row);
        ctx->Unmap(tex, 0);
        job.sink->Push(std::move(pixels));
        ++job.framesRead;
    }

    // 2. Render the next frames into the job's target, then put the live
    //    camera (and clip time) back before anything else renders the scene.
    if (job.nextFrame >= n || job.nextFrame - job.framesRead >= slots) return;

    const float liveYaw = scene->GetCameraYaw(), livePitch = scene->GetCameraPitch();
    const float liveDistance = scene->GetCameraDistance(), liveFocal = scene->GetCameraFocalHeight();
    const bool stepTime = (d.flags & kStepTime) != 0;
    const float liveTime = stepTime ? mgr.GetAnimationState(d.actor).currentTime : 0.0f;

    Result result = Result::Success;
    for (uint32_t i = 0; i < d.framesPerTick && job.nextFrame < n && job.nextFrame - job.framesRead < slots; ++i) {
        const auto& f = job.frames[job.nextFrame];
        scene->SetCamera(f.yaw, job.pitch, job.distance, job.focal);
        if (stepTime) mgr.SeekAnimation(d.actor, f.time);
        result = mgr.RenderSceneInto(d.scene, job.target);
        if (Failed(result)) break;
        ctx->CopyResource(job.staging[job.nextFrame % slots].Get(), job.target.GetTexture());
        ++job.nextFrame;
    }

    scene->SetCamera(liveYaw, livePitch, liveDistance, liveFocal);
    if (stepTime) mgr.SeekAnimation(d.actor, liveTime);
    if (Failed(result)) FinishJob(job, State::Failed, result);
}

void TurntableExporter::Tick() {
    struct Note {
        CompleteCallback callback;
        uint32_t job;
        SceneHandle scene;
        State state;
        Result result;
        std::string path;
    };
    std::vector<Note> notes;
    {
        std::lock_guard lock(mutex_);
        tickQueued_ = false;

        bool more = false;
        for (auto& [id, ptr] : jobs_) {
            auto& job = *ptr;
            if (job.state == State::Rendering) {
                // An encoder that gave up (unwritable path) ends the job early.
                Result sinkResult;
                if (job.sink && job.sink->Finished(sinkResult)) {
                    FinishJob(job, State::Failed, Failed(sinkResult) ? sinkResult : Result::ExportFailed);
                } else {
                    TickJob(job);
                    if (job.state == State::Rendering && job.framesRead == job.desc.frameCount) {
                        job.sink->Close();
                        job.ReleaseGpu();
                        job.state = State::Writing;
                    }
                }
            }
            if (job.state == State::Writing) {
                Result sinkResult;
                if (job.sink->Finished(sinkResult)) {
                    if (Failed(sinkResult)) FinishJob(job, State::Failed, sinkResult);
                    else FinishJob(job, State::Done, Result::Success);
                }
            }

            const bool finished = job.state == State::Done || job.state == State::Failed ||
                                  job.state == State::Cancelled;
            if (finished && !job.notified) {
                job.notified = true;
                finishedOrder_.push_back(id);
                if (job.state == State::Done) {
                    SKSE::log::info("[SkyrimRenderer] Export job {} done", id);
                }
                auto cb = callbacks_.find(job.owner);
                if (cb != callbacks_.end()) {
                    notes.push_back({cb->second, id, job.desc.scene, job.state, job.result,
                                     ExportEncoders::PathToUtf8(job.desc.outPath)});
                }
            }
            more |= !job.notified;
        }

        // Keep the last few finished statuses for polling; the rest go.
        while (finishedOrder_.size() > kKeepFinished) {
            jobs_.erase(finishedOrder_.front());
            finishedOrder_.erase(finishedOrder_.begin());
        }
        if (more) EnsureTicking();
    }

    // Outside the lock: callbacks call back into the API.
    for (const auto& note : notes) note.callback(note.job, note.scene, note.state, note.result, note.path);
}

// ============================================================================
// High-res export
// ============================================================================

Result ExportPNGScaled(SceneHandle sceneHandle, const std::filesystem::path& path, float scale) {
    auto& mgr = SceneManager::GetInstance();
    auto* scene = mgr.GetScene(sceneHandle);
    if (!scene || !scene->GetDevice()) return Result::InvalidHandle;
    if (path.empty() || !(scale > 0.0f) || !std::isfinite(scale)) return Result::InvalidParameter;
    if (mgr.IsDeviceLost()) return Result::DeviceLost;

    const double w0 = scene->GetWidth(), h0 = scene->GetHeight();
    if (w0 <= 0.0 || h0 <= 0.0) return Result::InvalidHandle;

    // Cap the output: 32768 a side, 2^28 pixels (1 GiB RGBA in memory).
    double s = scale;
    s = std::min(s, kMaxOutputSide / std::max(w0, h0));
    s = std::min(s, std::sqrt(static_cast<double>(kMaxOutputPixels) / (w0 * h0)));
    if (s < scale) {
        SKSE::log::warn("[SkyrimRenderer] ExportPNGScaled: scale {:.3f} lowered to {:.3f} (output cap)", scale, s);
    }
    const uint32_t W = std::max(1u, static_cast<uint32_t>(std::lround(w0 * s)));
    const uint32_t H = std::max(1u, static_cast<uint32_t>(std::lround(h0 * s)));

    // One target up to kMaxFrameSide a side (D3D11 allows 16384, but target +
    // depth + two staging copies cost 16 bytes a pixel of VRAM); larger
    // outputs render in tiles of that size, each an off-centre slice of the
    // full frame's projection, stitched here.
    static_assert(kMaxFrameSide <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    const uint32_t tileW = std::min(W, kMaxFrameSide), tileH = std::min(H, kMaxFrameSide);
    const uint32_t nx = (W + tileW - 1) / tileW, ny = (H + tileH - 1) / tileH;

    RenderTargetManager target;
    if (Failed(target.Initialize(scene->GetDevice(), tileW, tileH))) {
        SKSE::log::error("[SkyrimRenderer] ExportPNGScaled: could not create a {}x{} target", tileW, tileH);
        return Result::OutOfMemory;
    }

    std::vector<uint8_t> image;
    try {
        image.resize(static_cast<size_t>(W) * H * 4);
    } catch (const std::bad_alloc&) {
        return Result::OutOfMemory;
    }

    auto* ctx = scene->GetDeviceContext();
    std::vector<uint8_t> tile;
    for (uint32_t ty = 0; ty < ny; ++ty) {
        for (uint32_t tx = 0; tx < nx; ++tx) {
            const uint32_t x0 = tx * tileW, y0 = ty * tileH;
            ProjectionWindow window;
            if (nx * ny > 1) {
                // Edge tiles keep the full tile size and run past the image;
                // the overhang is cropped below. NDC y is up, rows go down.
                window.aspect = static_cast<float>(W) / static_cast<float>(H);
                window.x0 = -1.0f + 2.0f * static_cast<float>(x0) / static_cast<float>(W);
                window.x1 = -1.0f + 2.0f * static_cast<float>(x0 + tileW) / static_cast<float>(W);
                window.y1 = 1.0f - 2.0f * static_cast<float>(y0) / static_cast<float>(H);
                window.y0 = 1.0f - 2.0f * static_cast<float>(y0 + tileH) / static_cast<float>(H);
            }
            Result r = mgr.RenderSceneInto(sceneHandle, target, window);
            if (Failed(r)) return r;
            uint32_t tw = 0, th = 0;
            r = target.ReadbackPixels(ctx, tile, tw, th);
            if (Failed(r)) return r;

            const uint32_t cw = std::min(tileW, W - x0), ch = std::min(tileH, H - y0);
            for (uint32_t row = 0; row < ch; ++row) {
                std::memcpy(&image[(static_cast<size_t>(y0 + row) * W + x0) * 4],
                            &tile[static_cast<size_t>(row) * tw * 4], static_cast<size_t>(cw) * 4);
            }
        }
    }
    target.Release();

    const Result result = ExportEncoders::WritePNG(path, image.data(), W, H);
    if (Succeeded(result)) {
        SKSE::log::info("[SkyrimRenderer] Exported scaled PNG: {} ({}x{}, {} tile(s))", ExportEncoders::PathToUtf8(path),
                        W, H, nx * ny);
    }
    return result;
}

} // namespace SkyrimRenderer
