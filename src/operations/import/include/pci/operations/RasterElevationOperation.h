#pragma once

#include <pci/operations/LoadJobMechanics.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationRow.h>
#include <pci/operations/OperationTarget.h>
#include <pci/raster/RasterTileSource.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/tasking/TaskScheduler.h>

#include <pci/foundation/Generation.h>
#include <pci/foundation/LayerIdentity.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class OperationTarget;

enum class RasterElevationJobPhase : std::uint8_t {
    Queued,
    Scanning,
    Ready,
    Failed,
    Cancelled,
};

struct RasterElevationBindingToken {
    SceneLayerId layerId;
    RasterSourceId sourceId;
    BindingGeneration bindingGeneration;

    bool operator==(const RasterElevationBindingToken &) const = default;
};

struct RasterElevationJobState {
    LoadJobId jobId;
    SceneLayerId layerId;
    RasterSourceId sourceId;
    BindingGeneration bindingGeneration;
    RasterElevationJobPhase phase = RasterElevationJobPhase::Queued;
    std::uint64_t processedBlocks = 0;
    std::uint64_t totalBlocks = 0;
    std::string detail;
};

// Serializes exact DEM scans so a pair of large rasters cannot occupy both
// non-preemptive import workers. Tile streaming remains on its own workers.
class RasterElevationOperation {
public:
    explicit RasterElevationOperation(
        TaskScheduler &scheduler, std::shared_ptr<CompletionExecutor> executor);
    virtual ~RasterElevationOperation();

    [[nodiscard]] LoadJobId start(RasterElevationBindingToken token,
                                  RasterTileSourcePtr source);
    void cancelLayer(SceneLayerId layerId);
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool retry(LoadJobId jobId);
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::vector<RasterElevationJobState> jobStates() const;
    [[nodiscard]] std::vector<OperationRow>
    jobRows(std::optional<LoadJobId> only = std::nullopt) const;
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

public:
    struct Events {
        std::function<void(pci::LoadJobId,
                           pci::RasterElevationBindingToken,
                           pci::RasterElevationRange)>
            completed;
        std::function<void(
            pci::LoadJobId, pci::RasterElevationBindingToken, std::string)>
            failed;
        std::function<void(pci::LoadJobId, pci::RasterElevationBindingToken)>
            cancelled;
        std::function<void(pci::LoadJobId)> jobStateChanged;
    };
    void recoverDelivery()
    {
        callbackTarget_->recover();
    }
    void setEvents(Events events)
    {
        events_ = std::move(events);
    }
    void bindRegistry(OperationRegistry &registry,
                      std::shared_ptr<LoadJobIdSequence> ids,
                      OperationRegistration::Controls controls)
    {
        setJobIds(std::move(ids));
        registration_.bind(registry, std::move(controls));
    }
    void setJobIds(std::shared_ptr<LoadJobIdSequence> ids)
    {
        if (!jobs_.empty())
            throw std::logic_error(
                "cannot replace active job identity allocator");
        jobIds_ = std::move(ids);
    }

private:
    Events events_;
    OperationRegistration registration_;
    void notifyCompleted(pci::LoadJobId jobId,
                         pci::RasterElevationBindingToken token,
                         pci::RasterElevationRange range)
    {
        if (events_.completed)
            events_.completed(jobId, token, range);
    }
    void notifyFailed(pci::LoadJobId jobId,
                      pci::RasterElevationBindingToken token,
                      std::string message)
    {
        if (events_.failed)
            events_.failed(jobId, token, message);
    }
    void notifyCancelled(pci::LoadJobId jobId,
                         pci::RasterElevationBindingToken token)
    {
        if (events_.cancelled)
            events_.cancelled(jobId, token);
    }
    void notifyJobStateChanged(pci::LoadJobId jobId)
    {
        const auto rows = jobRows(jobId);
        if (rows.empty())
            registration_.remove(jobId);
        else
            registration_.publish(rows.front());
        if (events_.jobStateChanged)
            events_.jobStateChanged(jobId);
    }

private:
    struct Job {
        RasterElevationBindingToken token;
        RasterTileSourcePtr source;
        OperationLifecycle lifecycle;
        RasterElevationJobPhase phase = RasterElevationJobPhase::Queued;
    };

    void scheduleNext();
    void observeProgress(LoadJobId jobId,
                         AttemptGeneration generation,
                         RasterElevationScanProgress progress);
    void finish(LoadJobId jobId,
                AttemptGeneration generation,
                std::optional<RasterElevationRange> range,
                std::string error,
                bool cancelled);

    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<OperationTarget<RasterElevationOperation>> callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, RasterElevationJobState> states_;
    std::unordered_map<SceneLayerId, LoadJobId> layerJobs_;
    std::deque<LoadJobId> pending_;
    std::optional<LoadJobId> active_;
    std::shared_ptr<LoadJobIdSequence> jobIds_ =
        std::make_shared<LoadJobIdSequence>();
};

} // namespace pci
