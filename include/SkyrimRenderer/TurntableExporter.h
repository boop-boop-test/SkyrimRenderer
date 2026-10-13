#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/SceneManager.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// Multi-frame exports: turntables (camera yaw stepped around the orbit
/// centre) and animation strips (an actor's clip stepped with SeekAnimation),
/// written as a sprite sheet + JSON, numbered PNGs, an APNG or a GIF.
///
/// A job renders a few frames per Tick (SceneManager's tick queue, so it runs
/// wherever the scene's consumer drives Tick: the Present hook or its own
/// main-thread SkyrimRenderer_Tick) into its own render target, so the live
/// scene's camera, pose and texture are restored before anything else sees
/// them. Readback is pipelined through two staging textures (no GPU stall)
/// and encoding runs on a per-job thread. Completion is reported through the
/// owning consumer's callback (on the Tick thread) and a pollable status.
class TurntableExporter {
public:
    enum class Format : uint32_t { Sheet = 0, Frames = 1, APNG = 2, GIF = 3 };
    enum class State : uint32_t { Unknown = 0, Rendering = 1, Writing = 2, Done = 3, Failed = 4, Cancelled = 5 };

    static constexpr uint32_t kStepYaw = 0x1;
    static constexpr uint32_t kStepTime = 0x2;
    static constexpr uint32_t kInclusiveEnd = 0x4;
    static constexpr uint32_t kMaxFrames = 1024;

    struct Desc {
        SceneHandle scene = SceneHandle::Invalid;
        ActorHandle actor = ActorHandle::Invalid;   // kStepTime: whose clip is stepped
        Format format = Format::Sheet;
        uint32_t flags = kStepYaw;
        uint32_t frameCount = 0;
        float startYaw = 0.0f, endYaw = 0.0f;        // equal: one full turn
        float startTime = 0.0f, endTime = 0.0f;      // endTime <= startTime: to the clip's end
        uint32_t width = 0, height = 0;              // 0: the scene's size
        uint32_t frameDelayMs = 0;                   // animated formats; 0: automatic
        uint32_t loopCount = 0;                      // animated formats; 0 = forever
        uint32_t columns = 0;                        // sheet; 0: about square
        uint32_t framesPerTick = 0;                  // 0: 1
        std::filesystem::path outPath;
    };

    struct Status {
        State state = State::Unknown;
        Result result = Result::Success;
        SceneHandle scene = SceneHandle::Invalid;
        uint32_t framesRendered = 0;
        uint32_t framesWritten = 0;
        uint32_t frameCount = 0;
    };

    /// job, scene, final state, result, output path (UTF-8)
    using CompleteCallback = std::function<void(uint32_t, SceneHandle, State, Result, const std::string&)>;

    static TurntableExporter& Get();

    /// Validate and queue a job. Returns its id, 0 when refused (see log).
    uint32_t Start(const Desc& desc, ConsumerId owner);

    bool GetStatus(uint32_t job, Status& out);
    void Cancel(uint32_t job);
    /// Forget a job (cancelling it if it still runs; no callback then).
    void Release(uint32_t job);

    void SetCompleteCallback(CompleteCallback callback, ConsumerId consumer);

    /// Drop `consumer`'s jobs and callback.
    void Shutdown(ConsumerId consumer);

    ~TurntableExporter();

private:
    TurntableExporter() = default;
    TurntableExporter(const TurntableExporter&) = delete;
    TurntableExporter& operator=(const TurntableExporter&) = delete;

    struct Job;
    class FrameSink;

    void EnsureTicking();       // caller holds mutex_
    void Tick();                // SceneManager tick task
    void TickJob(Job& job);     // render + collect; caller holds mutex_
    void FinishJob(Job& job, State state, Result result);

    std::mutex mutex_;
    uint32_t nextJob_ = 1;
    bool tickQueued_ = false;
    std::unordered_map<uint32_t, std::unique_ptr<Job>> jobs_;
    std::vector<uint32_t> finishedOrder_;   // finished job ids, oldest first (bounded)
    std::unordered_map<ConsumerId, CompleteCallback> callbacks_;
};

/// Render a scene once at `scale` x its size into temporary targets and write
/// a compressed PNG; the live scene (size, texture, camera) is untouched.
/// Larger than 4096 px on a side renders in tiles (an off-centre projection
/// per tile, stitched on the CPU). The output is capped at 32768 px a side and
/// 2^28 pixels (scale is lowered to fit). Synchronous; render thread only.
Result ExportPNGScaled(SceneHandle scene, const std::filesystem::path& path, float scale);

} // namespace SkyrimRenderer
