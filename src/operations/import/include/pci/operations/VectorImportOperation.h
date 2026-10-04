#pragma once

#include <pci/foundation/JobResult.h>
#include <pci/operations/LoadJobMechanics.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationRow.h>
#include <pci/operations/OperationTarget.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/tasking/TaskScheduler.h>

#include <pci/operations/VectorImport.h>

#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class OperationTarget;

enum class VectorLoadJobPhase : std::uint8_t {
    Queued,
    Inspecting,
    AwaitingChoice,
    ResolvingOrigin,
    Reading,
    Ready,
    Failed,
    Cancelled,
};

struct VectorLoadJobState {
    LoadJobId jobId;
    VectorLoadJobPhase phase = VectorLoadJobPhase::Queued;
    VectorLoadSummary summary;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    std::string detail;
    bool canCancel = true;
    bool canRetry = false;
};

class VectorImportOperation {
public:
    VectorImportOperation(std::shared_ptr<const VectorLoader> loader,
                          TaskScheduler &scheduler,
                          std::shared_ptr<CompletionExecutor> executor);
    virtual ~VectorImportOperation();

    // Inspection and loading share one logical job and one task-row identity.
    [[nodiscard]] LoadJobId startInspection(VectorImportRequest request);
    [[nodiscard]] bool continueLoad(LoadJobId jobId,
                                    std::vector<VectorSublayerKey> selected);
    [[nodiscard]] bool retry(LoadJobId jobId, SessionGeneration session = {});
    using Installer =
        std::function<JobResult<void>(LoadJobId,
                                      SessionGeneration,
                                      AttemptGeneration,
                                      const VectorSublayerKey &,
                                      const VectorLayerDataPtr &)>;
    void setInstaller(Installer installer)
    {
        installer_ = std::move(installer);
    }
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::optional<VectorLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<VectorLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<OperationRow>
    jobRows(std::optional<LoadJobId> only = std::nullopt) const;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

public:
    struct Events {
        std::function<void(pci::LoadJobId, pci::VectorImportPreflight)>
            inspected;
        std::function<void(pci::LoadJobId, std::uint64_t, std::uint64_t)>
            progressChanged;
        std::function<void(
            pci::LoadJobId, pci::VectorSublayerKey, pci::VectorLayerDataPtr)>
            sublayerLoaded;
        std::function<void(pci::LoadJobId, pci::VectorSublayerKey, std::string)>
            sublayerFailed;
        std::function<void(pci::LoadJobId, pci::VectorLoadSummary)> loaded;
        std::function<void(pci::LoadJobId, pci::VectorLoadSummary)> finished;
        std::function<void(pci::LoadJobId, std::string)> failed;
        std::function<void(pci::LoadJobId, pci::VectorLoadSummary)> cancelled;
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
    void notifyInspected(pci::LoadJobId jobId,
                         pci::VectorImportPreflight preflight)
    {
        if (events_.inspected)
            events_.inspected(jobId, preflight);
    }
    void notifyProgressChanged(pci::LoadJobId jobId,
                               std::uint64_t processed,
                               std::uint64_t total)
    {
        if (events_.progressChanged)
            events_.progressChanged(jobId, processed, total);
    }
    void notifySublayerLoaded(pci::LoadJobId jobId,
                              pci::VectorSublayerKey key,
                              pci::VectorLayerDataPtr data)
    {
        if (events_.sublayerLoaded)
            events_.sublayerLoaded(jobId, key, data);
    }
    void notifySublayerFailed(pci::LoadJobId jobId,
                              pci::VectorSublayerKey key,
                              std::string message)
    {
        if (events_.sublayerFailed)
            events_.sublayerFailed(jobId, key, message);
    }
    void notifyLoaded(pci::LoadJobId jobId, pci::VectorLoadSummary summary)
    {
        if (events_.loaded)
            events_.loaded(jobId, summary);
    }
    void notifyFinished(pci::LoadJobId jobId, pci::VectorLoadSummary summary)
    {
        if (events_.finished)
            events_.finished(jobId, summary);
    }
    void notifyFailed(pci::LoadJobId jobId, std::string message)
    {
        if (events_.failed)
            events_.failed(jobId, message);
    }
    void notifyCancelled(pci::LoadJobId jobId, pci::VectorLoadSummary summary)
    {
        if (events_.cancelled)
            events_.cancelled(jobId, summary);
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
    friend class VectorLoadControllerTestAccess;
    using InspectionResult = JobResult<VectorImportPreflight>;
    using OriginResult = JobResult<std::array<double, 2>>;
    using SublayerResult = JobResult<VectorLayerDataPtr>;

    struct Job {
        VectorImportRequest request;
        std::optional<VectorImportPreflight> preflight;
        OperationLifecycle lifecycle;
        VectorLoadSummary summary;
        std::size_t outstanding = 0;
        std::size_t nextSublayer = 0;
        std::vector<bool> received;
        VectorLoadJobPhase phase = VectorLoadJobPhase::Queued;
        bool cancelled = false;
        // One entry for each currently scheduled sublayer. Keeping these
        // separately lets a concurrent scheduler publish a monotonic aggregate
        // without assuming that feature counts are available.
        std::vector<VectorImportProgress> sublayerProgress;
    };
    [[nodiscard]] LoadJobId createJob(VectorImportRequest request,
                                      VectorLoadJobPhase phase);
    void scheduleInspection(LoadJobId jobId);
    void finishInspection(LoadJobId jobId,
                          AttemptGeneration generation,
                          InspectionResult result);
    void beginLoad(LoadJobId jobId);
    void scheduleSublayers(LoadJobId jobId);
    void finishOrigin(LoadJobId jobId,
                      AttemptGeneration generation,
                      OriginResult result);
    void finishOne(LoadJobId jobId,
                   VectorSublayerKey key,
                   AttemptGeneration generation,
                   SublayerResult result);
    void reportProgress(LoadJobId jobId,
                        const VectorSublayerKey &key,
                        AttemptGeneration generation,
                        VectorImportProgress progress);
    void setState(LoadJobId jobId,
                  VectorLoadJobPhase phase,
                  std::string detail = {});
    void emitTerminal(LoadJobId jobId);
    std::shared_ptr<const VectorLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<OperationTarget<VectorImportOperation>> callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, VectorLoadJobState> states_;
    std::shared_ptr<LoadJobIdSequence> jobIds_ =
        std::make_shared<LoadJobIdSequence>();
    Installer installer_;
};

} // namespace pci
