#pragma once

#include <pci/foundation/JobResult.h>
#include <pci/operations/LoadJobMechanics.h>
#include <pci/operations/OperationLifecycle.h>
#include <pci/operations/OperationRegistration.h>
#include <pci/operations/OperationRow.h>
#include <pci/operations/OperationTarget.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/tasking/TaskScheduler.h>

#include <pci/operations/RasterImport.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class OperationTarget;

enum class RasterLoadJobPhase : std::uint8_t {
    Queued,
    Inspecting,
    SamplingRange,
    Ready,
    Failed,
    Cancelled,
};

struct RasterLoadJobState {
    LoadJobId jobId;
    RasterLoadJobPhase phase = RasterLoadJobPhase::Queued;
    std::string detail;
};

// One job per selected path, so a failure in one source preserves the others.
// Inspection reads no full-resolution payload; it may perform a bounded window
// sample to derive a display range.
class RasterImportOperation {
public:
    RasterImportOperation(std::shared_ptr<const RasterLoader> loader,
                          TaskScheduler &scheduler,
                          std::shared_ptr<CompletionExecutor> executor);
    virtual ~RasterImportOperation();

    [[nodiscard]] LoadJobId startImport(RasterImportRequest request);
    [[nodiscard]] bool retry(LoadJobId jobId, SessionGeneration session = {});
    // Synchronous owner-thread prepare/commit acknowledgment. Standalone
    // consumers may leave this empty and consume loaded() data directly.
    using Installer = std::function<JobResult<void>(LoadJobId,
                                                    SessionGeneration,
                                                    AttemptGeneration,
                                                    const RasterLayerDataPtr &,
                                                    bool)>;
    void setInstaller(Installer installer)
    {
        installer_ = std::move(installer);
    }
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::optional<RasterLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<RasterLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<OperationRow>
    jobRows(std::optional<LoadJobId> only = std::nullopt) const;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

public:
    struct Events {
        std::function<void(pci::LoadJobId, pci::RasterLayerDataPtr, bool)>
            loaded;
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
    void notifyLoaded(pci::LoadJobId jobId,
                      pci::RasterLayerDataPtr data,
                      bool initiallyVisible)
    {
        if (events_.loaded)
            events_.loaded(jobId, data, initiallyVisible);
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
    friend class RasterLoadControllerTestAccess;
    using InspectionResult = JobResult<RasterImportPreflight>;

    struct Job {
        RasterImportRequest request;
        OperationLifecycle lifecycle;
        RasterLoadJobPhase phase = RasterLoadJobPhase::Queued;
        bool cancelled = false;
    };

    [[nodiscard]] LoadJobId createJob(RasterImportRequest request);
    void scheduleInspection(LoadJobId jobId);
    void finishInspection(LoadJobId jobId,
                          AttemptGeneration generation,
                          InspectionResult result);
    void observePhase(LoadJobId jobId,
                      AttemptGeneration generation,
                      RasterImportPhase phase);
    void setState(LoadJobId jobId,
                  RasterLoadJobPhase phase,
                  std::string detail = {});
    void emitTerminal(LoadJobId jobId, std::string message);

    std::shared_ptr<const RasterLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<OperationTarget<RasterImportOperation>> callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, RasterLoadJobState> states_;
    std::shared_ptr<LoadJobIdSequence> jobIds_ =
        std::make_shared<LoadJobIdSequence>();
    Installer installer_;
};

} // namespace pci
