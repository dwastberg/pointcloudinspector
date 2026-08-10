#pragma once

#include "raster/RasterTileCache.h"
#include "renderer/planning/RasterLodPlanner.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pci {

// Never let pending work grow without bound: source size and catalog
// cardinality must not translate into queue growth.
inline constexpr std::size_t rasterMaximumPendingRequests = 256;
inline constexpr std::uint32_t rasterDefaultReadWorkers = 2;
inline constexpr std::uint64_t rasterDefaultCpuCacheBytes =
    256ULL * 1024 * 1024;
inline constexpr std::uint64_t rasterDefaultGpuCacheBytes =
    256ULL * 1024 * 1024;
// Uploading more than this in one frame trades a stall for latency the user
// notices more.
inline constexpr std::uint64_t rasterFrameUploadBytes = 32ULL * 1024 * 1024;
inline constexpr std::size_t rasterMaximumFrameUploads = 64;

struct RasterStreamerMetrics {
    std::uint64_t requested = 0;
    std::uint64_t completed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t admitted = 0;
    // Completed reads the decoded cache had no room for, because every
    // resident tile was protected as on-screen.
    std::uint64_t rejectedUnadmitted = 0;
    std::uint64_t cpuBytes = 0;
    std::uint64_t cpuPeakBytes = 0;
    std::uint64_t cacheEvictions = 0;
    // Accepted and not yet completed, whether waiting for a worker or being
    // read. `queued` is the subset still waiting, so the two must never be
    // added together.
    std::size_t pending = 0;
    std::size_t queued = 0;
    std::size_t pendingUploads = 0;
    std::size_t negativeEntries = 0;
};

// Combines immutable source defaults with the scene style for one render
// generation. Scalar ramps are resolved by stable catalog key and copied once
// into shared storage that remains alive until every worker using it exits.
[[nodiscard]] std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayer &layer,
                              const PointColorMapCatalogSnapshot &colorMaps);

// One visible layer's contribution to a frame's reconciliation.
//
// The read queue is frame-global, so reconciliation must see every visible
// layer at once. Reconciling one layer at a time makes each layer's
// cancellation sweep drop the other layers' queued work, because a request for
// a different source is never in the current layer's wanted set: with two
// raster layers the queue thrashes every frame and only reads already picked
// up by a worker ever finish.
struct RasterFrameLayer {
    const RasterLodPlan *plan = nullptr;
    const RasterLayer *layer = nullptr;
    // Resolved from the layer style once per render generation. Keeping it on
    // the frame entry prevents the streamer from silently falling back to the
    // source default when the user edits a scalar range or color ramp.
    std::shared_ptr<const RasterDecodeParameters> decode;
};

// Owns request generations, worker scheduling, cancellation, decoded-cache
// admission, and render-thread wakeups. It never creates QRhi resources on a
// worker: results cross back through a queue the render thread drains.
//
// Its workers are deliberately separate from the import TaskScheduler, so a
// long-running point-cloud import cannot starve tile reads and leave the
// viewport blank.
class RasterTileStreamer {
public:
    using WakeCallback = std::function<void()>;

    explicit RasterTileStreamer(
        std::uint64_t cpuByteBudget,
        std::uint32_t workerCount = rasterDefaultReadWorkers);
    ~RasterTileStreamer();

    RasterTileStreamer(const RasterTileStreamer &) = delete;
    RasterTileStreamer &operator=(const RasterTileStreamer &) = delete;

    // Invoked from a worker when a result arrives. Coalesced with an atomic
    // flag, because one event per completed tile would flood the event loop
    // during a large refinement.
    void setWakeCallback(WakeCallback wake);
    void setCpuByteBudget(std::uint64_t bytes,
                          std::span<const RasterCacheKey> protectedKeys);
    [[nodiscard]] std::uint64_t cpuByteBudget() const noexcept;

    // Render thread only. Cancels queued work outside the frame's plans and
    // schedules their requests in priority order. Every visible layer must be
    // passed in one call; see RasterFrameLayer.
    void reconcile(std::span<const RasterFrameLayer> frame);
    // Single-layer convenience for the common case and for tests.
    void reconcile(const RasterLodPlan &plan, const RasterLayer &layer);

    // Render thread only. Admits finished reads into the decoded cache and
    // returns the keys newly awaiting GPU upload. Bounded per call so a burst
    // of completions cannot stall a frame.
    [[nodiscard]] std::vector<RasterCacheKey>
    drainCompletions(std::span<const RasterCacheKey> protectedKeys,
                     std::size_t maximumResults = 64);
    [[nodiscard]] std::vector<RasterCacheKey>
    takeReadyUploads(std::size_t maximumTiles);
    // Returns GPU-rejected uploads to the bounded ready queue. A CPU-resident
    // tile must remain retryable when the GPU cache is temporarily full of
    // protected on-screen tiles; dropping the key strands it forever because
    // the planner correctly declines to decode it again.
    void requeueReadyUploads(std::span<const RasterCacheKey> keys);

    [[nodiscard]] bool cpuResident(const RasterCacheKey &key) const noexcept;
    [[nodiscard]] const RasterTileData *tile(const RasterCacheKey &key);
    // A failed read is remembered for this source generation so it is reported
    // once rather than retried every frame. Cancellation never lands here.
    [[nodiscard]] bool failed(const RasterCacheKey &key) const noexcept;
    [[nodiscard]] std::optional<std::string>
    lastError(RasterSourceId sourceId) const;

    void releaseSource(RasterSourceId sourceId);
    // Stops workers and drops queued work. Must run before the sources and the
    // viewport it reports to are destroyed.
    void shutdown() noexcept;
    // Qualification callers may need to drain completions while reservations
    // apply backpressure. Preserve their current on-screen set during that
    // internal admission just as a render-frame drain would.
    void waitForIdle(std::span<const RasterCacheKey> protectedKeys = {});

    [[nodiscard]] RasterStreamerMetrics metrics() const;

private:
    struct Request {
        RasterCacheKey key;
        std::uint64_t epoch = 0;
        RasterTileSourcePtr source;
        RasterTileRequest request;
    };

    struct Completion {
        RasterCacheKey key;
        std::uint64_t epoch = 0;
        std::optional<RasterTileData> tile;
        std::string error;
        bool cancelled = false;
        // A successful completion keeps its destination reservation until the
        // allocation is transferred into the decoded cache or destroyed.
        std::uint64_t reservedBytes = 0;
    };

    void workerLoop(std::stop_token stop);
    void notifyWake();
    void releaseCompletionReservation(std::uint64_t bytes);
    void updateCpuPeak(std::uint64_t reservedBytes) noexcept;
    [[nodiscard]] std::uint64_t
    readPoolCapacity(std::uint64_t totalBudget) const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable_any queueReady_;
    std::deque<Request> queue_;
    std::unordered_set<RasterCacheKey> pendingKeys_;
    std::vector<Completion> completions_;
    // The current generation for sources still owned by the document. Workers
    // consult it before publishing, so a completion that races layer removal
    // or a range/ramp edit is destroyed before render-thread admission.
    std::unordered_map<RasterSourceId, std::uint64_t> liveGenerations_;
    std::stop_source stop_;
    std::vector<std::jthread> workers_;
    std::size_t activeReads_ = 0;
    std::size_t memoryWaiters_ = 0;
    std::condition_variable_any idle_;
    std::condition_variable_any memoryReady_;
    std::uint64_t totalCpuByteBudget_ = 0;
    std::uint64_t readPoolByteCapacity_ = 0;
    // Active source scratch plus decoded completions not yet transferred to
    // the cache. Guarded by mutex_.
    std::uint64_t reservedReadBytes_ = 0;
    std::uint32_t workerCount_ = 1;

    // Render-thread state; not guarded by mutex_.
    RasterTileCache cache_;
    std::uint64_t requestEpoch_ = 0;
    std::vector<RasterCacheKey> pendingUploads_;
    std::unordered_map<RasterCacheKey, std::string> negativeCache_;
    RasterStreamerMetrics metrics_;

    // Lets worker-side reservation changes update the combined peak without
    // touching the render-thread-owned cache object.
    std::atomic_uint64_t cacheResidentBytes_{0};
    std::atomic_uint64_t cpuPeakBytes_{0};

    std::atomic_bool wakeQueued_{false};
    WakeCallback wake_;
};

} // namespace pci
