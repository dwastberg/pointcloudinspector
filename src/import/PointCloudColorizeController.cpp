#include "import/PointCloudColorizeController.h"

#include "foundation/CheckedArithmetic.h"
#include "import/QueuedControllerCallback.h"
#include "scene/RasterColorizeRunStore.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace pci {
namespace {

template <typename Completion>
void post(
    const std::shared_ptr<QueuedControllerTarget<PointCloudColorizeController>>
        &target,
    Completion completion)
{
    postToObject(target, std::move(completion));
}

[[nodiscard]] bool terminal(const PointCloudColorizeJobPhase phase) noexcept
{
    return phase == PointCloudColorizeJobPhase::Ready ||
           phase == PointCloudColorizeJobPhase::Failed ||
           phase == PointCloudColorizeJobPhase::Cancelled;
}

[[nodiscard]] PointMemoryBudget::ReservationPtr
reserve(const PointMemoryBudgetPtr &budget,
        const std::uint64_t bytes,
        const char *purpose,
        const bool rebake)
{
    const auto reservation = budget->tryReserve(bytes);
    if (!reservation) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::InsufficientPointMemory,
            std::string("Insufficient point memory for ") + purpose +
                " (additional " + std::to_string(bytes) + " bytes required)" +
                (rebake ? ". Revert to Source Colors first to release the "
                          "current color table, then try again"
                        : ""));
    }
    return *reservation;
}

} // namespace

PointCloudColorizeController::PointCloudColorizeController(
    TaskScheduler &scheduler, QObject *parent)
    : QObject(parent)
    , scheduler_(&scheduler)
    , callbackTarget_(
          std::make_shared<
              QueuedControllerTarget<PointCloudColorizeController>>(this))
{
    qRegisterMetaType<LoadJobId>();
    qRegisterMetaType<RasterColorizeCommitToken>();
    qRegisterMetaType<RasterColorizePreparedPtr>();
    qRegisterMetaType<RasterPointColorBinding>();
}

PointCloudColorizeController::~PointCloudColorizeController()
{
    callbackTarget_->invalidate();
    cancelAll();
}

void PointCloudColorizeController::WorkerCompletion::finish()
{
    {
        const std::scoped_lock lock(mutex);
        done = true;
    }
    condition.notify_all();
}

void PointCloudColorizeController::WorkerCompletion::wait()
{
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] {
        return done;
    });
}

void PointCloudColorizeController::prepareJob(Job &job)
{
    if (!job.request.token.scene || !job.request.raster ||
        !job.request.decode || !job.request.memoryBudget) {
        throw std::invalid_argument("Colorization request is incomplete");
    }
    const RasterPointColorizeAvailability availability =
        job.request.token.scene->rasterPointColorizeAvailability();
    if (availability != RasterPointColorizeAvailability::Ready) {
        throw RasterColorizeError(
            availability == RasterPointColorizeAvailability::Loading
                ? RasterColorizeFailureCode::Loading
                : RasterColorizeFailureCode::UnsupportedSource,
            availability == RasterPointColorizeAvailability::Loading
                ? "Wait for this point-cloud import to finish"
                : "This source is read directly and has no local page cache to "
                  "colorize");
    }
    auto target = job.request.token.scene->rasterPointColorizeTarget();
    if (!target) {
        throw RasterColorizeError(RasterColorizeFailureCode::SourceChanged,
                                  "Point-cloud colorization target changed");
    }
    RasterColorizePreflight preflight =
        preflightRasterPointColorize(std::move(*target),
                                     job.request.raster->metadata(),
                                     job.request.options);
    const bool rebake = job.request.token.scene->hasRasterPointColors();
    const auto colorBytes = checkedMultiply(
        preflight.tableEntries, std::uint64_t{sizeof(std::uint32_t)});
    if (!colorBytes) {
        throw RasterColorizeError(RasterColorizeFailureCode::TooManyPoints,
                                  "Raster color table size overflows");
    }
    auto colorReservation = reserve(
        job.request.memoryBudget, *colorBytes, "the color table", rebake);
    auto workingReservation = reserve(job.request.memoryBudget,
                                      preflight.workingReservationBytes,
                                      "colorization working memory",
                                      rebake);
    auto rootStagingReservation = reserve(job.request.memoryBudget,
                                          preflight.rootStagingReservationBytes,
                                          "the colored hierarchy root",
                                          rebake);
    auto flatStagingReservation = reserve(job.request.memoryBudget,
                                          preflight.flatStagingReservationBytes,
                                          "the flat replacement blocks",
                                          rebake);
    job.preflight = std::move(preflight);
    job.colorReservation = std::move(colorReservation);
    job.workingReservation = std::move(workingReservation);
    job.rootStagingReservation = std::move(rootStagingReservation);
    job.flatStagingReservation = std::move(flatStagingReservation);
}

void PointCloudColorizeController::refreshPreflightAfterAdmission(Job &job)
{
    auto target = job.request.token.scene->rasterPointColorizeTarget();
    if (!target) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::SourceChanged,
            "Point-cloud colorization target changed during memory admission");
    }
    RasterColorizePreflight refreshed =
        preflightRasterPointColorize(std::move(*target),
                                     job.request.raster->metadata(),
                                     job.request.options);
    const auto colorBytes = checkedMultiply(
        refreshed.tableEntries, std::uint64_t{sizeof(std::uint32_t)});
    if (!colorBytes || !job.colorReservation->tryResize(*colorBytes) ||
        !job.workingReservation->tryResize(refreshed.workingReservationBytes) ||
        !job.rootStagingReservation->tryResize(
            refreshed.rootStagingReservationBytes) ||
        !job.flatStagingReservation->tryResize(
            refreshed.flatStagingReservationBytes)) {
        throw RasterColorizeError(
            RasterColorizeFailureCode::InsufficientPointMemory,
            "Point-memory requirements changed during colorization admission");
    }
    job.preflight = std::move(refreshed);
}

LoadJobId PointCloudColorizeController::startColorize(
    PointCloudColorizeRequest request,
    std::function<void()> synchronizeAdmission)
{
    if (hasActiveJob(request.token.pointLayerId)) {
        throw std::invalid_argument(
            "Colorization is already running for this layer");
    }
    Job job;
    job.request = std::move(request);
    job.synchronizeAdmission = std::move(synchronizeAdmission);
    prepareJob(job);
    if (job.synchronizeAdmission) {
        job.synchronizeAdmission();
        refreshPreflightAfterAdmission(job);
    }
    const LoadJobId jobId =
        jobIds_.next("point-cloud colorization job ids are exhausted");
    states_.emplace(jobId,
                    PointCloudColorizeJobState{
                        .jobId = jobId,
                        .pointLayerId = job.request.token.pointLayerId,
                        .rasterLayerId = job.request.token.rasterLayerId,
                        .pointLayerName = job.request.pointLayerName,
                        .rasterLayerName = job.request.rasterLayerName,
                        .phase = PointCloudColorizeJobPhase::Queued,
                        .detail = QStringLiteral("Waiting to colorize"),
                        .completion = 0.0,
                    });
    jobs_.emplace(jobId, std::move(job));
    emit jobStateChanged(jobId);
    schedule(jobId);
    return jobId;
}

void PointCloudColorizeController::schedule(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    Job &job = found->second;
    const std::uint64_t generation = job.generation;
    const auto callbackTarget = callbackTarget_;
    RasterColorizePreflight preflight = job.preflight;
    const PointCloudColorizeRequest request = job.request;
    auto colorReservation = job.colorReservation;
    auto workingReservation = job.workingReservation;
    auto rootReservation = job.rootStagingReservation;
    auto flatReservation = job.flatStagingReservation;
    job.workerCompletion = std::make_shared<WorkerCompletion>();
    const auto workerCompletion = job.workerCompletion;
    const std::stop_token stop = job.stop.get_token();

    job.task = scheduler_->submit(
        TaskPriority::Background,
        1,
        [callbackTarget,
         jobId,
         generation,
         preflight = std::move(preflight),
         request,
         colorReservation = std::move(colorReservation),
         workingReservation = std::move(workingReservation),
         rootReservation = std::move(rootReservation),
         flatReservation = std::move(flatReservation),
         workerCompletion,
         stop]() mutable {
            struct CompletionGuard {
                std::shared_ptr<WorkerCompletion> completion;
                ~CompletionGuard()
                {
                    completion->finish();
                }
            } completionGuard{workerCompletion};
            std::optional<RasterColorizePreparedPtr> result;
            QString failure;
            try {
                result = colorizePointCloudFromRaster(
                    std::move(preflight),
                    request.raster,
                    request.decode,
                    request.token.rasterRenderGeneration,
                    request.options,
                    std::move(colorReservation),
                    std::move(workingReservation),
                    std::move(rootReservation),
                    std::move(flatReservation),
                    stop,
                    [callbackTarget, jobId, generation](
                        const RasterColorizeProgress value) {
                        post(callbackTarget,
                             [jobId, generation, value](
                                 PointCloudColorizeController *controller) {
                                 controller->observeProgress(
                                     jobId, generation, value);
                             });
                    });
            } catch (const std::exception &error) {
                failure = QString::fromStdString(error.what());
            }
            post(callbackTarget,
                 [jobId,
                  generation,
                  result = std::move(result),
                  failure = std::move(failure)](
                     PointCloudColorizeController *controller) mutable {
                     controller->finishWorker(jobId,
                                              generation,
                                              std::move(result),
                                              std::move(failure));
                 });
        },
        [callbackTarget, jobId, generation, workerCompletion] {
            workerCompletion->finish();
            post(callbackTarget,
                 [jobId, generation](PointCloudColorizeController *controller) {
                     controller->finishWorker(
                         jobId, generation, std::nullopt, {});
                 });
        },
        job.request.token.pointLayerId.value());
}

void PointCloudColorizeController::observeProgress(
    const LoadJobId jobId,
    const std::uint64_t generation,
    const RasterColorizeProgress progress)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation ||
        terminal(found->second.phase)) {
        return;
    }
    found->second.latestProgress = progress;
    const double fraction =
        progress.total == 0 ? 1.0
                            : std::min(1.0,
                                       static_cast<double>(progress.completed) /
                                           static_cast<double>(progress.total));
    if (progress.phase == RasterColorizePhase::BuildingRecords) {
        setState(jobId,
                 PointCloudColorizeJobPhase::BuildingRecords,
                 QStringLiteral("Reading points and sorting sample locations"),
                 fraction * 0.45);
    } else {
        setState(jobId,
                 PointCloudColorizeJobPhase::SamplingRaster,
                 QStringLiteral("Sampling raster tiles (%1 tiles read)")
                     .arg(progress.tileReads),
                 0.45 + fraction * 0.5);
    }
}

void PointCloudColorizeController::finishWorker(
    const LoadJobId jobId,
    const std::uint64_t generation,
    std::optional<RasterColorizePreparedPtr> result,
    QString failure)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation) {
        return;
    }
    Job &job = found->second;
    job.colorReservation.reset();
    job.workingReservation.reset();
    job.rootStagingReservation.reset();
    job.flatStagingReservation.reset();
    if (job.stop.stop_requested() || (!result && failure.isEmpty())) {
        setState(jobId,
                 PointCloudColorizeJobPhase::Cancelled,
                 QStringLiteral("Cancelled"));
        emitTerminal(jobId);
        return;
    }
    if (!failure.isEmpty()) {
        setState(jobId, PointCloudColorizeJobPhase::Failed, failure);
        emitTerminal(jobId, failure);
        return;
    }
    if (!*result) {
        const QString message =
            QStringLiteral("Colorization produced no prepared result");
        setState(jobId, PointCloudColorizeJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }

    RasterColorizePreparedPtr preparedResult = std::move(*result);
    const RasterColorizeStatistics statistics = preparedResult->statistics;
    job.completedStatistics = statistics;
    RasterPointColorBinding binding{
        .rasterLayerId = job.request.token.rasterLayerId,
        .rasterSourceId = job.request.token.rasterSourceId,
        .rasterSourcePath = job.request.raster->metadata().sourcePath,
        .decode = job.request.decode,
        .rasterRenderGeneration = job.request.token.rasterRenderGeneration,
        .coloredPoints = statistics.pointsColored,
        .uncoloredPoints = statistics.pointsUnchanged,
        .crsRelation = job.request.crsRelation,
    };
    setState(jobId,
             PointCloudColorizeJobPhase::Committing,
             QStringLiteral("Applying colors"),
             0.95);
    emit prepared(jobId,
                  job.request.token,
                  std::move(preparedResult),
                  std::move(binding));
}

void PointCloudColorizeController::finishCommit(const LoadJobId jobId,
                                                const bool applied,
                                                QString failureMessage)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != PointCloudColorizeJobPhase::Committing) {
        return;
    }
    if (!applied) {
        if (failureMessage.isEmpty()) {
            failureMessage = QStringLiteral("Layer state changed while colors "
                                            "were being prepared; try again");
        }
        setState(jobId, PointCloudColorizeJobPhase::Failed, failureMessage);
        emitTerminal(jobId, failureMessage);
        return;
    }
    setState(jobId,
             PointCloudColorizeJobPhase::Ready,
             QStringLiteral("%1 colored · %2 kept source color")
                 .arg(found->second.completedStatistics.pointsColored)
                 .arg(found->second.completedStatistics.pointsUnchanged),
             1.0);
    found->second.terminalEmitted = true;
}

bool PointCloudColorizeController::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != PointCloudColorizeJobPhase::Failed) {
        return false;
    }
    Job &job = found->second;
    ++job.generation;
    job.stop = std::stop_source{};
    job.terminalEmitted = false;
    job.latestProgress = {};
    job.completedStatistics = {};
    try {
        prepareJob(job);
        if (job.synchronizeAdmission) {
            job.synchronizeAdmission();
            refreshPreflightAfterAdmission(job);
        }
    } catch (const std::exception &error) {
        job.colorReservation.reset();
        job.workingReservation.reset();
        job.rootStagingReservation.reset();
        job.flatStagingReservation.reset();
        setState(jobId,
                 PointCloudColorizeJobPhase::Failed,
                 QString::fromUtf8(error.what()));
        return false;
    }
    setState(jobId,
             PointCloudColorizeJobPhase::Queued,
             QStringLiteral("Waiting to colorize"),
             0.0);
    schedule(jobId);
    return true;
}

void PointCloudColorizeController::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    found->second.stop.request_stop();
    static_cast<void>(scheduler_->cancel(found->second.task));
}

void PointCloudColorizeController::cancelAll()
{
    std::vector<LoadJobId> active;
    for (const auto &[id, job] : jobs_) {
        if (!terminal(job.phase)) {
            active.push_back(id);
        }
    }
    for (const LoadJobId id : active) {
        cancel(id);
    }
}

void PointCloudColorizeController::cancelForPointLayer(
    const PointCloudLayerId layerId)
{
    for (const auto &[id, job] : jobs_) {
        if (job.request.token.pointLayerId == layerId && !terminal(job.phase)) {
            cancel(id);
        }
    }
}

void PointCloudColorizeController::cancelAndWaitForPointLayer(
    const PointCloudLayerId layerId)
{
    std::vector<std::shared_ptr<WorkerCompletion>> completions;
    std::vector<LoadJobId> jobs;
    for (const auto &[id, job] : jobs_) {
        if (job.request.token.pointLayerId == layerId && !terminal(job.phase)) {
            jobs.push_back(id);
            if (job.workerCompletion) {
                completions.push_back(job.workerCompletion);
            }
        }
    }
    for (const LoadJobId id : jobs) {
        cancel(id);
    }
    for (const auto &completion : completions) {
        completion->wait();
    }
}

void PointCloudColorizeController::cancelForRasterLayer(
    const SceneLayerId layerId)
{
    for (const auto &[id, job] : jobs_) {
        if (job.request.token.rasterLayerId == layerId &&
            !terminal(job.phase)) {
            cancel(id);
        }
    }
}

bool PointCloudColorizeController::hasActiveJob(
    const PointCloudLayerId layerId) const noexcept
{
    return std::ranges::any_of(jobs_, [layerId](const auto &entry) {
        return entry.second.request.token.pointLayerId == layerId &&
               !terminal(entry.second.phase);
    });
}

bool PointCloudColorizeController::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

bool PointCloudColorizeController::isCommitInProgress(
    const PointCloudLayerId layerId) const noexcept
{
    return std::ranges::any_of(jobs_, [layerId](const auto &entry) {
        return entry.second.request.token.pointLayerId == layerId &&
               entry.second.phase == PointCloudColorizeJobPhase::Committing;
    });
}

std::optional<PointCloudColorizeJobState>
PointCloudColorizeController::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    return found == states_.end()
               ? std::optional<PointCloudColorizeJobState>{}
               : std::optional<PointCloudColorizeJobState>{found->second};
}

std::vector<PointCloudColorizeJobState>
PointCloudColorizeController::jobStates() const
{
    return sortedJobStates<PointCloudColorizeJobState>(states_);
}

std::vector<LoadJobRow> PointCloudColorizeController::jobRows() const
{
    std::vector<LoadJobRow> rows;
    for (const PointCloudColorizeJobState &state : jobStates()) {
        const bool isTerminal = terminal(state.phase);
        const QString target = state.pointLayerName.isEmpty()
                                   ? QStringLiteral("point cloud")
                                   : state.pointLayerName;
        const QString detail =
            state.rasterLayerName.isEmpty()
                ? state.detail
                : QStringLiteral("From %1 · %2")
                      .arg(state.rasterLayerName, state.detail);
        rows.push_back({
            .key = {.kind = LoadJobKind::Colorize, .id = state.jobId},
            .title = QStringLiteral("Colorize %1").arg(target),
            .detail = detail,
            .completion = state.completion,
            .terminal = isTerminal,
            .capabilities = isTerminal ? terminalLoadJobCapabilities(
                                             state.phase ==
                                             PointCloudColorizeJobPhase::Failed)
                                       : activeLoadJobCapabilities(),
        });
    }
    return rows;
}

PointCloudColorizeControllerMetrics
PointCloudColorizeController::metrics() const noexcept
{
    PointCloudColorizeControllerMetrics result;
    for (const auto &[id, job] : jobs_) {
        static_cast<void>(id);
        if (terminal(job.phase)) {
            continue;
        }
        const auto addReservation =
            [](std::uint64_t &destination,
               const PointMemoryBudget::ReservationPtr &reservation) {
                destination = saturatingAdd(
                    destination, reservation ? reservation->bytes() : 0);
            };
        addReservation(result.activeColorTableBytes, job.colorReservation);
        addReservation(result.activeStagingBytes, job.rootStagingReservation);
        addReservation(result.activeStagingBytes, job.flatStagingReservation);
        result.activeRunBufferBytes = saturatingAdd(
            result.activeRunBufferBytes,
            saturatingMultiply(
                std::min(job.request.options.maximumScatterRecords,
                         job.preflight.maximumRecordCount),
                std::uint64_t{sizeof(RasterSortRecord)}));
        result.temporaryBytesWritten =
            saturatingAdd(result.temporaryBytesWritten,
                          job.latestProgress.temporaryBytesWritten);
        if (job.latestProgress.phase == RasterColorizePhase::SamplingRaster) {
            result.recordsGenerated = saturatingAdd(result.recordsGenerated,
                                                    job.latestProgress.total);
            result.recordsSampled = saturatingAdd(result.recordsSampled,
                                                  job.latestProgress.completed);
        }
        result.rasterTilesAttempted = saturatingAdd(
            result.rasterTilesAttempted, job.latestProgress.tileReads);
        result.rasterTilesFailed = saturatingAdd(
            result.rasterTilesFailed, job.latestProgress.tileFailures);
    }
    return result;
}

bool PointCloudColorizeController::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !terminal(found->second.phase)) {
        return false;
    }
    jobs_.erase(found);
    states_.erase(jobId);
    emit jobStateChanged(jobId);
    return true;
}

const TaskScheduler *
PointCloudColorizeController::schedulerIdentity() const noexcept
{
    return scheduler_;
}

void PointCloudColorizeController::setState(
    const LoadJobId jobId,
    const PointCloudColorizeJobPhase phase,
    QString detail,
    const std::optional<double> completion)
{
    const auto job = jobs_.find(jobId);
    if (job != jobs_.end()) {
        job->second.phase = phase;
    }
    const auto state = states_.find(jobId);
    if (state == states_.end()) {
        return;
    }
    state->second.phase = phase;
    if (!detail.isEmpty()) {
        state->second.detail = std::move(detail);
    }
    if (completion) {
        state->second.completion = *completion;
    }
    emit jobStateChanged(jobId);
}

void PointCloudColorizeController::emitTerminal(const LoadJobId jobId,
                                                QString message)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.terminalEmitted) {
        return;
    }
    found->second.terminalEmitted = true;
    if (found->second.phase == PointCloudColorizeJobPhase::Cancelled) {
        emit cancelled(jobId);
    } else {
        emit failed(jobId, std::move(message));
    }
}

} // namespace pci
