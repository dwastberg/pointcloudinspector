#pragma once

#include "import/JobResult.h"
#include "import/LoadJobMechanics.h"
#include "import/LoadJobRow.h"
#include "import/PointCloudImport.h"
#include "tasking/TaskScheduler.h"

#include <QMetaType>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

Q_DECLARE_METATYPE(pci::PointCloudScenePtr)
Q_DECLARE_METATYPE(pci::PointCloudImportStage)

namespace pci {

template <typename Controller> class QueuedControllerTarget;

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
    QString detail;
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

struct PointCloudLoadControllerMetrics {
    TaskSchedulerMetrics scheduler;
    std::uint64_t preflightCompleted = 0;
    std::uint64_t preflightFailed = 0;
    std::uint64_t safetySampledSources = 0;
    std::uint64_t admittedFlatReservationBytes = 0;
    std::uint64_t processResidentBytes = 0;
    std::uint64_t peakProcessResidentBytes = 0;
};

class PointCloudLoadController final : public QObject {
    Q_OBJECT

public:
    PointCloudLoadController(std::shared_ptr<const PointCloudLoader> loader,
                             TaskScheduler &scheduler,
                             QObject *parent = nullptr);
    ~PointCloudLoadController() override;

    // Single loads use the same inspect/admit/schedule path as a one-element
    // batch. No file count is special and no task uses Qt's global pool.
    LoadJobId load(PointCloudLoadOptions options);
    LoadJobId load(PointCloudLoadRequest request);
    [[nodiscard]] std::vector<LoadJobId>
    loadBatch(std::vector<PointCloudLoadRequest> requests);
    void cancel();                // cancel every in-flight job
    void cancel(LoadJobId jobId); // cancel a single job
    [[nodiscard]] bool prioritize(LoadJobId jobId);
    [[nodiscard]] std::optional<PointCloudLoadJobState>
    jobState(LoadJobId jobId) const;
    [[nodiscard]] std::vector<PointCloudLoadJobState> jobStates() const;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    // Completed records are retained for recovery/status UI until dismissed.
    // Active records cannot be dismissed.
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] PointCloudLoadControllerMetrics metrics() const;
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

signals:
    void progressChanged(pci::LoadJobId jobId,
                         pci::PointCloudImportStage stage,
                         quint64 processed,
                         quint64 total);
    void sceneReady(pci::LoadJobId jobId, pci::PointCloudScenePtr scene);
    void loaded(pci::LoadJobId jobId, pci::PointCloudScenePtr scene);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void schedulingChanged();
    void jobStateChanged(pci::LoadJobId jobId);

private:
    enum class JobPhase : std::uint8_t {
        Inspecting,
        WaitingForBatch,
        Loading,
    };

    using LoadOutcome = JobResult<PointCloudScenePtr>;
    using InspectionOutcome = JobResult<PointCloudImportPreflight>;
    struct Job {
        PointCloudLoadOptions options;
        PointCloudLoadResources resources;
        std::optional<PointCloudImportPreflight> preflight;
        std::stop_source stop;
        TaskScheduler::TaskId taskId;
        std::uint64_t batchId = 0;
        JobPhase phase = JobPhase::Inspecting;
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
    void finishQueuedCancellation(LoadJobId jobId, JobPhase phase);
    void
    removePreflightJob(LoadJobId jobId, bool cancelled, const QString &error);
    void advanceBatch(std::uint64_t batchId);
    void setJobState(LoadJobId jobId,
                     PointCloudLoadJobPhase phase,
                     std::uint64_t processed = 0,
                     std::uint64_t total = 0,
                     QString detail = {});

    std::shared_ptr<const PointCloudLoader> loader_;
    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<QueuedControllerTarget<PointCloudLoadController>>
        callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, PointCloudLoadJobState> jobStates_;
    std::unordered_map<std::uint64_t, Batch> batches_;
    LoadJobIdSequence jobIds_;
    std::uint64_t nextBatchId_ = 0;
    std::uint64_t preflightCompleted_ = 0;
    std::uint64_t preflightFailed_ = 0;
    std::uint64_t safetySampledSources_ = 0;
    std::uint64_t admittedFlatReservationBytes_ = 0;
    bool destroying_ = false;
};

} // namespace pci
