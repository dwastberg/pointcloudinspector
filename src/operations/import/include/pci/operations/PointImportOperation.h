#pragma once

#include <pci/foundation/JobResult.h>
#include <pci/operations/LoadJobMechanics.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationRow.h>
#include <pci/operations/OperationTarget.h>
#include <pci/operations/PointCloudLoader.h>
#include <pci/operations/PointDatasetIngestion.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/tasking/TaskScheduler.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class OperationTarget;

enum class PointCloudLoadJobPhase : std::uint8_t {
    Queued,
    Inspecting,
    WaitingForResources,
    Reading,
    Indexing,
    PreviewReady,
    Ready,
    Failed,
    Cancelled,
};

struct PointCloudLoadJobState {
    LoadJobId jobId;
    std::filesystem::path sourcePath;
    PointCloudLoadJobPhase phase = PointCloudLoadJobPhase::Queued;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    std::string detail;
    bool localPaging = false;
    bool safetySampled = false;
    bool previewAvailable = false;
    bool canCancel = true;
    bool canRetry = false;

    [[nodiscard]] double weightedCompletion() const noexcept;
};

struct PointCloudBatchProgress {
    double completion = 0.0;
    std::size_t sourceCount = 0;
    std::size_t completedSources = 0;
    std::size_t failedSources = 0;
    std::size_t cancelledSources = 0;
};

[[nodiscard]] PointCloudBatchProgress weightedBatchProgress(
    const std::vector<PointCloudLoadJobState> &states) noexcept;

struct PointImportOperationMetrics {
    TaskSchedulerMetrics scheduler;
    std::uint64_t preflightCompleted = 0;
    std::uint64_t preflightFailed = 0;
    std::uint64_t safetySampledSources = 0;
    std::uint64_t admittedFlatReservationBytes = 0;
    std::uint64_t processResidentBytes = 0;
    std::uint64_t peakProcessResidentBytes = 0;
};

class PointImportOperation {

public:
    PointImportOperation(std::shared_ptr<const PointCloudLoader> loader,
                         TaskScheduler &scheduler,
                         std::shared_ptr<CompletionExecutor> executor);
    virtual ~PointImportOperation();

    // Single loads use the same inspect/admit/schedule path as a one-element
    // batch. No file count is special and no task uses Qt's global pool.
    LoadJobId load(PointCloudLoadOptions options);
    LoadJobId load(PointCloudLoadRequest request);
    [[nodiscard]] std::vector<LoadJobId>
    loadBatch(std::vector<PointCloudLoadRequest> requests);
    void rejectInstallation(LoadJobId jobId, const std::string &message);
    void cancel();                // cancel every in-flight job
    void cancel(LoadJobId jobId); // cancel a single job
    [[nodiscard]] bool prioritize(LoadJobId jobId);
    [[nodiscard]] std::optional<PointCloudLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<PointCloudLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<OperationRow>
    jobRows(std::optional<LoadJobId> only = std::nullopt) const;
    // Completed records are retained for recovery/status UI until dismissed.
    // Active records cannot be dismissed.
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] PointImportOperationMetrics operationMetrics() const;
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

public:
    struct Events {
        std::function<void(pci::LoadJobId,
                           pci::PointCloudImportStage,
                           std::uint64_t,
                           std::uint64_t)>
            progressChanged;
        std::function<void(pci::LoadJobId, pci::PointDatasetEvent)> dataReady;
        std::function<void(pci::LoadJobId, pci::PreparedPointDatasetPtr)>
            prepared;
        std::function<void(pci::LoadJobId, pci::PreparedPointDatasetPtr)>
            loaded;
        std::function<void(pci::LoadJobId, std::string)> failed;
        std::function<void(pci::LoadJobId)> cancelled;
        std::function<void()> schedulingChanged;
        std::function<void(pci::LoadJobId)> jobStateChanged;
    };
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
    void recoverDelivery()
    {
        callbackTarget_->recover();
        if (ingestionWakeFailed_->exchange(false))
            drainIngestion();
    }

private:
    Events events_;
    OperationRegistration registration_;
    void notifyProgressChanged(pci::LoadJobId jobId,
                               pci::PointCloudImportStage stage,
                               std::uint64_t processed,
                               std::uint64_t total)
    {
        if (events_.progressChanged)
            events_.progressChanged(jobId, stage, processed, total);
    }
    void notifyDataReady(pci::LoadJobId jobId, pci::PointDatasetEvent event)
    {
        if (events_.dataReady)
            events_.dataReady(jobId, event);
    }
    void notifyPrepared(pci::LoadJobId jobId,
                        pci::PreparedPointDatasetPtr dataset)
    {
        if (events_.prepared)
            events_.prepared(jobId, dataset);
    }
    void notifyLoaded(pci::LoadJobId jobId,
                      pci::PreparedPointDatasetPtr dataset)
    {
        if (events_.loaded)
            events_.loaded(jobId, dataset);
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
    void notifySchedulingChanged()
    {
        if (events_.schedulingChanged)
            events_.schedulingChanged();
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
    friend class PointCloudLoadControllerTestAccess;
    enum class JobPhase : std::uint8_t {
        Inspecting,
        WaitingForBatch,
        Loading,
    };

    using LoadOutcome = JobResult<PreparedPointDatasetPtr>;
    using InspectionOutcome = JobResult<PointCloudImportPreflight>;
    struct Job {
        PointCloudLoadOptions options;
        PointCloudLoadResources resources;
        std::optional<PointCloudImportPreflight> preflight;
        OperationLifecycle lifecycle;
        std::uint64_t batchId = 0;
        JobPhase phase = JobPhase::Inspecting;
        PointImportToken token;
        std::shared_ptr<PointDatasetIngestion> ingestion;
        std::uint64_t nextSequence = 0;
    };
    struct Batch {
        std::size_t remainingPreflights = 0;
    };

    [[nodiscard]] LoadJobId createJob(PointCloudLoadRequest request,
                                      std::uint64_t batchId);
    void scheduleInspection(LoadJobId jobId);
    void finishInspection(LoadJobId jobId, InspectionOutcome outcome);
    void prepareBatch(std::uint64_t batchId);
    void prepareMemoryAdmissions(const std::vector<LoadJobId> &jobIds);
    void scheduleLoad(LoadJobId jobId);
    void finishLoad(LoadJobId jobId, LoadOutcome outcome);
    void drainIngestion();
    void requestIngestionWake();
    void finishQueuedCancellation(LoadJobId jobId, JobPhase phase);
    void removePreflightJob(LoadJobId jobId,
                            bool cancelled,
                            const std::string &error);
    void advanceBatch(std::uint64_t batchId);
    void setJobState(LoadJobId jobId,
                     PointCloudLoadJobPhase phase,
                     std::uint64_t processed = 0,
                     std::uint64_t total = 0,
                     std::string detail = {});

    std::shared_ptr<const PointCloudLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<OperationTarget<PointImportOperation>> callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, PointCloudLoadJobState> jobStates_;
    std::unordered_map<std::uint64_t, Batch> batches_;
    std::shared_ptr<LoadJobIdSequence> jobIds_ =
        std::make_shared<LoadJobIdSequence>();
    std::uint64_t nextBatchId_ = 0;
    std::uint64_t preflightCompleted_ = 0;
    std::uint64_t preflightFailed_ = 0;
    std::uint64_t safetySampledSources_ = 0;
    std::uint64_t admittedFlatReservationBytes_ = 0;
    bool destroying_ = false;
    std::shared_ptr<std::atomic_bool> ingestionWakePending_ =
        std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> ingestionWakeFailed_ =
        std::make_shared<std::atomic_bool>(false);
    LoadJobId lastDrainedJob_;
};

} // namespace pci
