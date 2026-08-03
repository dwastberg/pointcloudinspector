#pragma once

#include "import/JobResult.h"
#include "import/LoadJobMechanics.h"
#include "import/LoadJobRow.h"
#include "tasking/TaskScheduler.h"
#include "vector/VectorImport.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class QueuedControllerTarget;

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
    QString detail;
    bool canCancel = true;
    bool canRetry = false;
};

class VectorLoadController final : public QObject {
    Q_OBJECT
public:
    VectorLoadController(std::shared_ptr<const VectorLoader> loader,
                         TaskScheduler &scheduler,
                         QObject *parent = nullptr);
    ~VectorLoadController() override;

    // Inspection and loading share one logical job and one task-row identity.
    [[nodiscard]] LoadJobId startInspection(VectorImportRequest request);
    [[nodiscard]] bool continueLoad(LoadJobId jobId,
                                    std::vector<VectorSublayerKey> selected);
    [[nodiscard]] bool retry(LoadJobId jobId);
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::optional<VectorLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<VectorLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

signals:
    void inspected(pci::LoadJobId jobId, pci::VectorImportPreflight preflight);
    void
    progressChanged(pci::LoadJobId jobId, quint64 processed, quint64 total);
    void sublayerLoaded(pci::LoadJobId jobId,
                        pci::VectorSublayerKey key,
                        pci::VectorLayerDataPtr data);
    void sublayerFailed(pci::LoadJobId jobId,
                        pci::VectorSublayerKey key,
                        QString message);
    void loaded(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void finished(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId, pci::VectorLoadSummary summary);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    using InspectionResult = JobResult<VectorImportPreflight>;
    using OriginResult = JobResult<std::array<double, 2>>;
    using SublayerResult = JobResult<VectorLayerDataPtr>;

    struct Job {
        VectorImportRequest request;
        std::optional<VectorImportPreflight> preflight;
        std::stop_source stop;
        VectorLoadSummary summary;
        std::size_t outstanding = 0;
        std::uint64_t generation = 0;
        VectorLoadJobPhase phase = VectorLoadJobPhase::Queued;
        bool cancelled = false;
        bool terminalEmitted = false;
        // One entry for each currently scheduled sublayer. Keeping these
        // separately lets a concurrent scheduler publish a monotonic aggregate
        // without assuming that feature counts are available.
        std::vector<VectorImportProgress> sublayerProgress;
        std::vector<TaskScheduler::TaskId> tasks;
    };
    [[nodiscard]] LoadJobId createJob(VectorImportRequest request,
                                      VectorLoadJobPhase phase);
    void scheduleInspection(LoadJobId jobId);
    void finishInspection(LoadJobId jobId,
                          std::uint64_t generation,
                          InspectionResult result);
    void beginLoad(LoadJobId jobId);
    void finishOrigin(LoadJobId jobId,
                      std::uint64_t generation,
                      OriginResult result);
    void finishOne(LoadJobId jobId,
                   VectorSublayerKey key,
                   std::uint64_t generation,
                   SublayerResult result);
    void reportProgress(LoadJobId jobId,
                        const VectorSublayerKey &key,
                        std::uint64_t generation,
                        VectorImportProgress progress);
    void
    setState(LoadJobId jobId, VectorLoadJobPhase phase, QString detail = {});
    void emitTerminal(LoadJobId jobId);
    std::shared_ptr<const VectorLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<QueuedControllerTarget<VectorLoadController>>
        callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, VectorLoadJobState> states_;
    LoadJobIdSequence jobIds_;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::VectorLayerDataPtr)
Q_DECLARE_METATYPE(pci::VectorSublayerKey)
Q_DECLARE_METATYPE(pci::VectorLoadSummary)
Q_DECLARE_METATYPE(pci::VectorImportPreflight)
