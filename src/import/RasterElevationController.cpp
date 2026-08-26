#include "import/RasterElevationController.h"

#include "import/QueuedControllerCallback.h"
#include "platform/QtPath.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace pci {
namespace {

template <typename Completion>
void post(const std::shared_ptr<
              QueuedControllerTarget<RasterElevationController>> &target,
          Completion completion)
{
    postToObject(target, std::move(completion));
}

[[nodiscard]] bool terminal(const RasterElevationJobPhase phase) noexcept
{
    return phase == RasterElevationJobPhase::Ready ||
           phase == RasterElevationJobPhase::Failed ||
           phase == RasterElevationJobPhase::Cancelled;
}

} // namespace

RasterElevationController::RasterElevationController(TaskScheduler &scheduler,
                                                     QObject *parent)
    : QObject(parent)
    , scheduler_(&scheduler)
    , callbackTarget_(std::make_shared<
                      QueuedControllerTarget<RasterElevationController>>(this))
{
    qRegisterMetaType<RasterElevationRange>();
}

RasterElevationController::~RasterElevationController()
{
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId RasterElevationController::start(const SceneLayerId layerId,
                                           RasterLayerDataPtr data)
{
    if (!data || !data->source || !data->metadata().elevation.available) {
        throw std::invalid_argument(
            "elevation analysis requires an eligible raster source");
    }
    if (const auto existing = layerJobs_.find(layerId);
        existing != layerJobs_.end()) {
        const auto state = states_.find(existing->second);
        if (state != states_.end() && !terminal(state->second.phase)) {
            return existing->second;
        }
    }

    const LoadJobId jobId =
        jobIds_.next("raster elevation job ids are exhausted");
    jobs_.emplace(jobId,
                  Job{
                      .layerId = layerId,
                      .data = std::move(data),
                      .stop = {},
                      .generation = 1,
                      .phase = RasterElevationJobPhase::Queued,
                      .task = std::nullopt,
                  });
    const Job &job = jobs_.at(jobId);
    states_.emplace(jobId,
                    RasterElevationJobState{
                        .jobId = jobId,
                        .layerId = layerId,
                        .sourceId = job.data->sourceId,
                        .detail = {},
                    });
    layerJobs_.insert_or_assign(layerId, jobId);
    pending_.push_back(jobId);
    emit jobStateChanged(jobId);
    scheduleNext();
    return jobId;
}

void RasterElevationController::scheduleNext()
{
    if (active_) {
        return;
    }
    while (!pending_.empty()) {
        const LoadJobId jobId = pending_.front();
        pending_.pop_front();
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end() || terminal(found->second.phase)) {
            continue;
        }
        Job &job = found->second;
        job.phase = RasterElevationJobPhase::Scanning;
        states_.at(jobId).phase = RasterElevationJobPhase::Scanning;
        active_ = jobId;
        emit jobStateChanged(jobId);

        const std::uint64_t generation = job.generation;
        const RasterTileSourcePtr source = job.data->source;
        const std::stop_token stop = job.stop.get_token();
        const auto callbackTarget = callbackTarget_;
        const auto cancelledCallback =
            [callbackTarget, jobId, generation] {
                post(callbackTarget,
                     [jobId, generation](RasterElevationController *controller) {
                         controller->finish(
                             jobId, generation, std::nullopt, {}, true);
                     });
            };
        job.task = scheduler_->submit(
            TaskPriority::Background,
            source->exactElevationScanReservationBytes(),
            [callbackTarget, jobId, generation, source, stop] {
                try {
                    const RasterElevationRange range =
                        source->exactElevationRange(
                            stop,
                            [callbackTarget, jobId, generation](
                                const RasterElevationScanProgress progress) {
                                post(callbackTarget,
                                     [jobId, generation, progress](
                                         RasterElevationController *controller) {
                                         controller->observeProgress(
                                             jobId, generation, progress);
                                     });
                            });
                    post(callbackTarget,
                         [jobId, generation, range](
                             RasterElevationController *controller) {
                             controller->finish(jobId,
                                                generation,
                                                range,
                                                {},
                                                false);
                         });
                } catch (const RasterReadCancelled &) {
                    post(callbackTarget,
                         [jobId, generation](
                             RasterElevationController *controller) {
                             controller->finish(
                                 jobId, generation, std::nullopt, {}, true);
                         });
                } catch (const std::exception &error) {
                    const QString message = QString::fromUtf8(error.what());
                    post(callbackTarget,
                         [jobId, generation, message](
                             RasterElevationController *controller) {
                             controller->finish(jobId,
                                                generation,
                                                std::nullopt,
                                                message,
                                                false);
                         });
                }
            },
            cancelledCallback,
            jobId.value());
        return;
    }
}

void RasterElevationController::observeProgress(
    const LoadJobId jobId,
    const std::uint64_t generation,
    const RasterElevationScanProgress progress)
{
    const auto job = jobs_.find(jobId);
    const auto state = states_.find(jobId);
    if (job == jobs_.end() || state == states_.end() ||
        job->second.generation != generation || terminal(job->second.phase)) {
        return;
    }
    state->second.processedBlocks = progress.processedBlocks;
    state->second.totalBlocks = progress.totalBlocks;
    emit jobStateChanged(jobId);
}

void RasterElevationController::finish(
    const LoadJobId jobId,
    const std::uint64_t generation,
    const std::optional<RasterElevationRange> range,
    QString error,
    const bool wasCancelled)
{
    const auto job = jobs_.find(jobId);
    const auto state = states_.find(jobId);
    if (job == jobs_.end() || state == states_.end() ||
        job->second.generation != generation || terminal(job->second.phase)) {
        return;
    }
    job->second.task.reset();
    if (active_ == jobId) {
        active_.reset();
    }
    if (wasCancelled) {
        job->second.phase = RasterElevationJobPhase::Cancelled;
        state->second.phase = RasterElevationJobPhase::Cancelled;
        emit cancelled(jobId, job->second.layerId);
    } else if (range) {
        job->second.phase = RasterElevationJobPhase::Ready;
        state->second.phase = RasterElevationJobPhase::Ready;
        state->second.processedBlocks = state->second.totalBlocks;
        emit completed(jobId,
                       job->second.layerId,
                       job->second.data->sourceId,
                       *range);
    } else {
        job->second.phase = RasterElevationJobPhase::Failed;
        state->second.phase = RasterElevationJobPhase::Failed;
        state->second.detail = std::move(error);
        emit failed(jobId,
                    job->second.layerId,
                    job->second.data->sourceId,
                    state->second.detail);
    }
    emit jobStateChanged(jobId);
    scheduleNext();
}

void RasterElevationController::cancelLayer(const SceneLayerId layerId)
{
    if (const auto found = layerJobs_.find(layerId); found != layerJobs_.end()) {
        cancel(found->second);
    }
}

void RasterElevationController::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    found->second.stop.request_stop();
    if (found->second.task && scheduler_->cancel(*found->second.task)) {
        found->second.task.reset();
        return;
    }
    if (found->second.phase == RasterElevationJobPhase::Queued) {
        finish(jobId, found->second.generation, std::nullopt, {}, true);
    }
}

void RasterElevationController::cancelAll()
{
    std::vector<LoadJobId> ids;
    ids.reserve(jobs_.size());
    for (const auto &[id, job] : jobs_) {
        if (!terminal(job.phase)) {
            ids.push_back(id);
        }
    }
    for (const LoadJobId id : ids) {
        cancel(id);
    }
}

bool RasterElevationController::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        (found->second.phase != RasterElevationJobPhase::Failed &&
         found->second.phase != RasterElevationJobPhase::Cancelled)) {
        return false;
    }
    Job &job = found->second;
    job.stop = std::stop_source{};
    ++job.generation;
    job.phase = RasterElevationJobPhase::Queued;
    RasterElevationJobState &state = states_.at(jobId);
    state.phase = RasterElevationJobPhase::Queued;
    state.processedBlocks = 0;
    state.totalBlocks = 0;
    state.detail.clear();
    pending_.push_back(jobId);
    emit jobStateChanged(jobId);
    scheduleNext();
    return true;
}

bool RasterElevationController::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !terminal(found->second.phase)) {
        return false;
    }
    layerJobs_.erase(found->second.layerId);
    jobs_.erase(found);
    states_.erase(jobId);
    emit jobStateChanged(jobId);
    return true;
}

bool RasterElevationController::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

std::vector<RasterElevationJobState>
RasterElevationController::jobStates() const
{
    std::vector<RasterElevationJobState> result;
    result.reserve(states_.size());
    for (const auto &[id, state] : states_) {
        result.push_back(state);
    }
    std::ranges::sort(result, {}, &RasterElevationJobState::jobId);
    return result;
}

std::vector<LoadJobRow> RasterElevationController::jobRows() const
{
    std::vector<LoadJobRow> rows;
    for (const RasterElevationJobState &state : jobStates()) {
        QString title = QStringLiteral("DEM elevation analysis");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path filename =
                job->second.data->metadata().sourcePath.filename();
            if (!filename.empty()) {
                title = pathToQString(filename);
            }
        }
        QString detail = state.detail;
        switch (state.phase) {
        case RasterElevationJobPhase::Queued:
            detail = QStringLiteral("Queued for exact elevation analysis");
            break;
        case RasterElevationJobPhase::Scanning:
            detail = QStringLiteral("Scanning exact elevation range");
            break;
        case RasterElevationJobPhase::Ready:
            detail = QStringLiteral("Elevation range ready");
            break;
        case RasterElevationJobPhase::Failed:
            break;
        case RasterElevationJobPhase::Cancelled:
            detail = QStringLiteral("Cancelled");
            break;
        }
        const double completion =
            state.totalBlocks == 0
                ? (state.phase == RasterElevationJobPhase::Ready ? 1.0 : 0.0)
                : static_cast<double>(state.processedBlocks) /
                      static_cast<double>(state.totalBlocks);
        const bool isTerminal = terminal(state.phase);
        rows.push_back({
            .key = {.kind = LoadJobKind::RasterElevation, .id = state.jobId},
            .title = std::move(title),
            .detail = std::move(detail),
            .completion = std::clamp(completion, 0.0, 1.0),
            .terminal = isTerminal,
            .capabilities =
                isTerminal
                    ? terminalLoadJobCapabilities(
                          state.phase == RasterElevationJobPhase::Failed)
                    : activeLoadJobCapabilities(),
        });
    }
    return rows;
}

const TaskScheduler *
RasterElevationController::schedulerIdentity() const noexcept
{
    return scheduler_;
}

} // namespace pci
