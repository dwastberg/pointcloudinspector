#pragma once

#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/rendering/PointFrameExecutor.h>
#include <pci/rendering/planning/RasterFrameCoordinator.h>
#include <pci/runtime/raster/RasterTileStreamer.h>

#include <functional>
#include <pci/rendering/RenderTelemetry.h>
#include <unordered_map>

namespace pci {

struct FrameContext {
    SessionGeneration session;
    SceneDocumentSnapshotPtr document;
    SceneRuntimeSnapshotPtr runtime;
};

struct FramePlanningInput {
    PointFrameInput points;
    RasterFrameInput rasters;
};

// Called synchronously inside the existing rendering callback. The backend
// owns its capture storage and GPU resources; this port never contains QRhi
// handles and cannot process events or transfer the frame to another thread.
struct FrameBackend {
    std::function<FrameContext()> context;
    std::function<FramePlanningInput()> capture;
    std::function<void(const PointFramePlan &, const RasterFramePlan &)>
        protect;
    std::function<void(const PointFrameResult &, const RasterFramePlan &)>
        submit;
    std::function<void(bool)> complete;
};

class FrameExecutor final {
public:
    void run(const FrameBackend &backend);
    void clear();
    [[nodiscard]] RenderTelemetryAccumulator &telemetry() noexcept
    {
        return telemetry_;
    }
    [[nodiscard]] PointFrameCoordinator &points() noexcept
    {
        return points_;
    }
    [[nodiscard]] RasterTileStreamer &rasters() noexcept
    {
        return rasters_;
    }

private:
    friend struct FrameExecutorTestAccess;
    std::size_t scratchGrowths_ = 0;
    std::size_t rasterIdentityBuilds_ = 0;
    void drain(const FrameContext &context);
    void finishScratch() noexcept;
    std::vector<DecodedPageKey> protectionScratch_;
    std::vector<RasterRequestBatch> requestScratch_;
    std::uint64_t scratchBudget_ = 0;
    SessionGeneration scratchSession_;
    DocumentGeneration scratchDocument_;
    std::weak_ptr<const SceneDocumentSnapshot> indexedDocument_;
    std::weak_ptr<const SceneRuntimeSnapshot> indexedRuntime_;
    std::uint64_t indexedRevision_ = 0;
    SessionGeneration indexedSession_;
    void protectPoints(const FrameContext &context,
                       std::span<const PointFrameTrimRequest> requests);
    PointFrameCoordinator points_;
    PointFrameExecutor pointEffects_;
    RasterFrameCoordinator rasterPlanner_;
    RasterTileStreamer rasters_{rasterDefaultCpuCacheBytes};
    std::vector<PointFrameTrimRequest> previousPoints_;
    std::vector<RasterCacheKey> previousRasters_;
    std::unordered_map<RasterSourceId,
                       std::pair<BindingGeneration, std::uint64_t>>
        rasterSources_;
    RenderTelemetryAccumulator telemetry_;
    std::size_t drainCursor_ = 0;
};

} // namespace pci
