#pragma once

#include "import/JobResult.h"
#include "import/LoadJobMechanics.h"
#include "import/LoadJobRow.h"
#include "import/RasterImport.h"
#include "tasking/TaskScheduler.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class QueuedControllerTarget;

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
    QString detail;
};

// One job per selected path, so a failure in one source preserves the others.
// Inspection reads no full-resolution payload; it may perform a bounded window
// sample to derive a display range.
class RasterLoadController final : public QObject {
    Q_OBJECT
public:
    RasterLoadController(std::shared_ptr<const RasterLoader> loader,
                         TaskScheduler &scheduler,
                         QObject *parent = nullptr);
    ~RasterLoadController() override;

    [[nodiscard]] LoadJobId startImport(RasterImportRequest request);
    [[nodiscard]] bool retry(LoadJobId jobId);
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::optional<RasterLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<RasterLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

signals:
    // Carries the already-inspected source so the UI thread never reopens it.
    void loaded(pci::LoadJobId jobId,
                pci::RasterLayerDataPtr data,
                bool initiallyVisible);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    using InspectionResult = JobResult<RasterImportPreflight>;

    struct Job {
        RasterImportRequest request;
        std::stop_source stop;
        std::uint64_t generation = 0;
        RasterLoadJobPhase phase = RasterLoadJobPhase::Queued;
        bool cancelled = false;
        bool terminalEmitted = false;
        std::vector<TaskScheduler::TaskId> tasks;
    };

    [[nodiscard]] LoadJobId createJob(RasterImportRequest request);
    void scheduleInspection(LoadJobId jobId);
    void finishInspection(LoadJobId jobId,
                          std::uint64_t generation,
                          InspectionResult result);
    void observePhase(LoadJobId jobId,
                      std::uint64_t generation,
                      RasterImportPhase phase);
    void
    setState(LoadJobId jobId, RasterLoadJobPhase phase, QString detail = {});
    void emitTerminal(LoadJobId jobId, QString message);

    std::shared_ptr<const RasterLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<QueuedControllerTarget<RasterLoadController>>
        callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, RasterLoadJobState> states_;
    LoadJobIdSequence jobIds_;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::RasterLayerDataPtr)
