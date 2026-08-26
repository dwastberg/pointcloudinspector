#pragma once

#include "import/LoadJobMechanics.h"
#include "import/LoadJobRow.h"
#include "raster/RasterTileSource.h"
#include "scene/SceneDocument.h"
#include "tasking/TaskScheduler.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace pci {

template <typename Controller> class QueuedControllerTarget;

enum class RasterElevationJobPhase : std::uint8_t {
    Queued,
    Scanning,
    Ready,
    Failed,
    Cancelled,
};

struct RasterElevationJobState {
    LoadJobId jobId;
    SceneLayerId layerId;
    RasterSourceId sourceId;
    RasterElevationJobPhase phase = RasterElevationJobPhase::Queued;
    std::uint64_t processedBlocks = 0;
    std::uint64_t totalBlocks = 0;
    QString detail;
};

// Serializes exact DEM scans so a pair of large rasters cannot occupy both
// non-preemptive import workers. Tile streaming remains on its own workers.
class RasterElevationController final : public QObject {
    Q_OBJECT
public:
    explicit RasterElevationController(TaskScheduler &scheduler,
                                       QObject *parent = nullptr);
    ~RasterElevationController() override;

    [[nodiscard]] LoadJobId start(SceneLayerId layerId,
                                  RasterLayerDataPtr data);
    void cancelLayer(SceneLayerId layerId);
    void cancel(LoadJobId jobId);
    void cancelAll();
    [[nodiscard]] bool retry(LoadJobId jobId);
    [[nodiscard]] bool dismiss(LoadJobId jobId);
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::vector<RasterElevationJobState> jobStates() const;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;
    [[nodiscard]] const TaskScheduler *schedulerIdentity() const noexcept;

signals:
    void completed(pci::LoadJobId jobId,
                   pci::SceneLayerId layerId,
                   pci::RasterSourceId sourceId,
                   pci::RasterElevationRange range);
    void failed(pci::LoadJobId jobId,
                pci::SceneLayerId layerId,
                pci::RasterSourceId sourceId,
                QString message);
    void cancelled(pci::LoadJobId jobId, pci::SceneLayerId layerId);
    void jobStateChanged(pci::LoadJobId jobId);

private:
    struct Job {
        SceneLayerId layerId;
        RasterLayerDataPtr data;
        std::stop_source stop;
        std::uint64_t generation = 1;
        RasterElevationJobPhase phase = RasterElevationJobPhase::Queued;
        std::optional<TaskScheduler::TaskId> task;
    };

    void scheduleNext();
    void observeProgress(LoadJobId jobId,
                         std::uint64_t generation,
                         RasterElevationScanProgress progress);
    void finish(LoadJobId jobId,
                std::uint64_t generation,
                std::optional<RasterElevationRange> range,
                QString error,
                bool cancelled);

    TaskScheduler *scheduler_ = nullptr;
    std::shared_ptr<QueuedControllerTarget<RasterElevationController>>
        callbackTarget_;
    std::unordered_map<LoadJobId, Job> jobs_;
    std::unordered_map<LoadJobId, RasterElevationJobState> states_;
    std::unordered_map<SceneLayerId, LoadJobId> layerJobs_;
    std::deque<LoadJobId> pending_;
    std::optional<LoadJobId> active_;
    LoadJobIdSequence jobIds_;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::RasterElevationRange)
