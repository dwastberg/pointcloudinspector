#pragma once

#include "foundation/SpatialReferenceComparator.h"
#include "import/LoadJobMechanics.h"
#include "import/LoadJobRow.h"
#include "scene/RasterPointColorizer.h"
#include "scene/SceneDocument.h"
#include "tasking/TaskScheduler.h"

#include <QObject>
#include <QString>

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

template <typename Controller> class QueuedControllerTarget;

struct RasterColorizeCommitToken {
    PointCloudLayerId pointLayerId;
    PointCloudScenePtr scene;
    std::uint64_t pointColorGeneration = 0;
    SceneLayerId rasterLayerId;
    RasterSourceId rasterSourceId;
    std::uint64_t rasterRenderGeneration = 0;
};

struct PointCloudColorizeRequest {
    RasterColorizeCommitToken token;
    RasterTileSourcePtr raster;
    std::shared_ptr<const RasterDecodeParameters> decode;
    RasterColorizeOptions options;
    PointMemoryBudgetPtr memoryBudget;
    std::optional<SpatialReferenceRelation> crsRelation;
    QString pointLayerName;
    QString rasterLayerName;
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
    QString pointLayerName;
    QString rasterLayerName;
    PointCloudColorizeJobPhase phase = PointCloudColorizeJobPhase::Queued;
    QString detail;
    double completion = 0.0;
};

struct PointCloudColorizeControllerMetrics {
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t activeStagingBytes = 0;
    std::uint64_t activeRunBufferBytes = 0;
    std::uint64_t temporaryBytesWritten = 0;
    std::uint64_t recordsGenerated = 0;
    std::uint64_t recordsSampled = 0;
    std::uint64_t rasterTilesAttempted = 0;
    std::uint64_t rasterTilesFailed = 0;
};

class PointCloudColorizeController final : public QObject {
    Q_OBJECT
public:
    explicit PointCloudColorizeController(TaskScheduler &scheduler,
                                          QObject *parent = nullptr);
    ~PointCloudColorizeController() override;

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
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    [[nodiscard]] PointCloudColorizeControllerMetrics metrics() const noexcept;
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;
    void
    finishCommit(LoadJobId jobId, bool applied, QString failureMessage = {});

signals:
    void prepared(pci::LoadJobId jobId,
                  pci::RasterColorizeCommitToken token,
                  pci::RasterColorizePreparedPtr result,
                  pci::RasterPointColorBinding binding);
    void failed(pci::LoadJobId jobId, QString message);
    void cancelled(pci::LoadJobId jobId);
    void jobStateChanged(pci::LoadJobId jobId);

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
        std::stop_source stop;
        TaskScheduler::TaskId task;
        std::uint64_t generation = 0;
        PointCloudColorizeJobPhase phase = PointCloudColorizeJobPhase::Queued;
        RasterColorizeProgress latestProgress;
        RasterColorizeStatistics completedStatistics;
        std::function<void()> synchronizeAdmission;
        std::shared_ptr<WorkerCompletion> workerCompletion;
        bool terminalEmitted = false;
    };

    void prepareJob(Job &job);
    void refreshPreflightAfterAdmission(Job &job);
    void schedule(LoadJobId jobId);
    void observeProgress(LoadJobId jobId,
                         std::uint64_t generation,
                         RasterColorizeProgress progress);
    void finishWorker(LoadJobId jobId,
                      std::uint64_t generation,
                      std::optional<RasterColorizePreparedPtr> result,
                      QString failure);
    void setState(LoadJobId jobId,
                  PointCloudColorizeJobPhase phase,
                  QString detail = {},
                  std::optional<double> completion = std::nullopt);
    void emitTerminal(LoadJobId jobId, QString message = {});

    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<QueuedControllerTarget<PointCloudColorizeController>>
        callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, PointCloudColorizeJobState> states_;
    LoadJobIdSequence jobIds_;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::RasterColorizeCommitToken)
Q_DECLARE_METATYPE(pci::RasterColorizePreparedPtr)
Q_DECLARE_METATYPE(pci::RasterPointColorBinding)
