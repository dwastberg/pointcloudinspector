#pragma once

#include <pci/foundation/Generation.h>
#include <pci/foundation/SpatialReferenceComparator.h>
#include <pci/operations/LoadJobMechanics.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationRow.h>
#include <pci/operations/OperationTarget.h>
#include <pci/operations/RasterPointColorizer.h>
#include <pci/raster/RasterPointColorBinding.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/tasking/TaskScheduler.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class OperationTarget;
class PointDatasetRuntime;

struct RasterColorizeCommitToken {
    PointCloudLayerId pointLayerId;
    PointCloudSourceId pointSourceId;
    BindingGeneration pointBindingGeneration;
    std::uint64_t pointColorGeneration = 0;
    SceneLayerId rasterLayerId;
    RasterSourceId rasterSourceId;
    BindingGeneration rasterBindingGeneration;
    std::uint64_t rasterRenderGeneration = 0;
    SessionGeneration session;
};

struct PointCloudColorizeRequest {
    RasterColorizeCommitToken token;
    std::shared_ptr<PointDatasetRuntime> pointRuntime;
    RasterTileSourcePtr raster;
    std::shared_ptr<const RasterDecodeParameters> decode;
    RasterColorizeOptions options;
    PointMemoryBudgetPtr memoryBudget;
    std::optional<SpatialReferenceRelation> crsRelation;
    std::string pointLayerName;
    std::string rasterLayerName;
};

enum class PointCloudColorizeJobPhase : std::uint8_t {
    Queued,
    BuildingRecords,
    SamplingRaster,
    Committing,
    Ready,
    Failed,
    Cancelled,
};

struct PointCloudColorizeJobState {
    LoadJobId jobId;
    PointCloudLayerId pointLayerId;
    SceneLayerId rasterLayerId;
    std::string pointLayerName;
    std::string rasterLayerName;
    PointCloudColorizeJobPhase phase = PointCloudColorizeJobPhase::Queued;
    std::string detail;
    double completion = 0.0;
};

struct RasterColorizeOperationMetrics {
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t activeStagingBytes = 0;
    std::uint64_t activeRunBufferBytes = 0;
    std::uint64_t temporaryBytesWritten = 0;
    std::uint64_t recordsGenerated = 0;
    std::uint64_t recordsSampled = 0;
    std::uint64_t rasterTilesAttempted = 0;
    std::uint64_t rasterTilesFailed = 0;
};

class RasterColorizeOperation {
public:
    explicit RasterColorizeOperation(
        TaskScheduler &scheduler,
        RasterColorizeRunStoreFactory runStoreFactory,
        std::shared_ptr<CompletionExecutor> executor);
    virtual ~RasterColorizeOperation();

    [[nodiscard]] LoadJobId
    startColorize(PointCloudColorizeRequest request,
                  std::function<void()> synchronizeAdmission = {});
    [[nodiscard]] bool retry(LoadJobId jobId);
    void cancel(LoadJobId jobId);
    void cancelAll();
    void cancelForPointLayer(PointCloudLayerId layerId);
    void cancelAndWaitForPointLayer(PointCloudLayerId layerId);
    void cancelForRasterLayer(SceneLayerId layerId);
    [[nodiscard]] bool hasActiveJob(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] bool
    isCommitInProgress(PointCloudLayerId layerId) const noexcept;
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::optional<PointCloudColorizeJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<PointCloudColorizeJobState> jobStates() const;
    [[nodiscard]] std::vector<OperationRow>
    jobRows(std::optional<LoadJobId> only = std::nullopt) const;
    [[nodiscard]] RasterColorizeOperationMetrics metrics() const noexcept;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;
    void finishCommit(LoadJobId jobId,
                      bool applied,
                      std::string failureMessage = {});

public:
    struct Events {
        std::function<void(pci::LoadJobId,
                           pci::RasterColorizeCommitToken,
                           pci::PointColorInstallationPtr,
                           pci::RasterPointColorBinding)>
            prepared;
        std::function<void(pci::LoadJobId, std::string)> failed;
        std::function<void(pci::LoadJobId)> cancelled;
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
    void notifyPrepared(pci::LoadJobId jobId,
                        pci::RasterColorizeCommitToken token,
                        pci::PointColorInstallationPtr installation,
                        pci::RasterPointColorBinding binding)
    {
        if (events_.prepared)
            events_.prepared(jobId, token, installation, binding);
    }
    void notifyFailed(pci::LoadJobId jobId, std::string message)
    {
        if (events_.failed)
            events_.failed(jobId, message);
    }
    void notifyCancelled(pci::LoadJobId jobId)
    {
        if (events_.cancelled)
            events_.cancelled(jobId);
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
    struct WorkerCompletion {
        void finish();
        void wait();

        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
    };

    struct Job {
        PointCloudColorizeRequest request;
        RasterColorizePreflight preflight;
        PointMemoryBudget::ReservationPtr colorReservation;
        PointMemoryBudget::ReservationPtr workingReservation;
        PointMemoryBudget::ReservationPtr rootStagingReservation;
        PointMemoryBudget::ReservationPtr flatStagingReservation;
        OperationLifecycle lifecycle;
        PointCloudColorizeJobPhase phase = PointCloudColorizeJobPhase::Queued;
        RasterColorizeProgress latestProgress;
        RasterColorizeStatistics completedStatistics;
        std::function<void()> synchronizeAdmission;
        std::shared_ptr<WorkerCompletion> workerCompletion;
    };

    void prepareJob(Job &job);
    void refreshPreflightAfterAdmission(Job &job);
    void schedule(LoadJobId jobId);
    void observeProgress(LoadJobId jobId,
                         AttemptGeneration generation,
                         RasterColorizeProgress progress);
    void finishWorker(LoadJobId jobId,
                      AttemptGeneration generation,
                      std::optional<RasterColorizePreparedPtr> result,
                      std::string failure);
    void setState(LoadJobId jobId,
                  PointCloudColorizeJobPhase phase,
                  std::string detail = {},
                  std::optional<double> completion = std::nullopt);
    void emitTerminal(LoadJobId jobId, std::string message = {});

    TaskScheduler *scheduler_ = nullptr;
    RasterColorizeRunStoreFactory runStoreFactory_;
    std::shared_ptr<OperationTarget<RasterColorizeOperation>> callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, PointCloudColorizeJobState> states_;
    std::shared_ptr<LoadJobIdSequence> jobIds_ =
        std::make_shared<LoadJobIdSequence>();
};

} // namespace pci
