#include <pci/operations/RasterColorizeOperation.h>

#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/operations/OperationTarget.h>

#include <pci/operations/RasterColorizeRunStore.h>

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

namespace pci {
namespace {

template <typename Target, typename Completion>
void post(const std::shared_ptr<Target> &target, Completion completion)
{
    postToOperation(target, std::move(completion));
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

RasterColorizeOperation::RasterColorizeOperation(
    TaskScheduler &scheduler,
    RasterColorizeRunStoreFactory runStoreFactory,
    std::shared_ptr<CompletionExecutor> executor)
    : scheduler_(&scheduler)
    , runStoreFactory_(std::move(runStoreFactory))
    , callbackTarget_(
          std::make_shared<OperationTarget<RasterColorizeOperation>>(
              this, std::move(executor)))
{
    if (!runStoreFactory_) {
        throw std::invalid_argument(
            "point colorization requires a run-store factory");
    }
}

RasterColorizeOperation::~RasterColorizeOperation()
{
    events_ = {};
    registration_.detach();
    callbackTarget_->invalidate();
    cancelAll();
}

void RasterColorizeOperation::WorkerCompletion::finish()
{
    {
        const std::scoped_lock lock(mutex);
        done = true;
    }
    condition.notify_all();
}

void RasterColorizeOperation::WorkerCompletion::wait()
{
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] {
        return done;
    });
}

void RasterColorizeOperation::prepareJob(Job &job)
{
    if (!job.request.pointRuntime || !job.request.raster ||
        !job.request.decode || !job.request.memoryBudget) {
        throw std::invalid_argument("Colorization request is incomplete");
    }
    const RasterPointColorizeAvailability availability =
        job.request.pointRuntime->rasterPointColorizeAvailability();
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
    auto target = job.request.pointRuntime->rasterPointColorizeTarget();
    if (!target) {
        throw RasterColorizeError(RasterColorizeFailureCode::SourceChanged,
                                  "Point-cloud colorization target changed");
    }
    RasterColorizePreflight preflight =
        preflightRasterPointColorize(std::move(*target),
                                     job.request.raster->metadata(),
                                     job.request.options);
    const bool rebake = job.request.pointRuntime->hasRasterPointColors();
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

void RasterColorizeOperation::refreshPreflightAfterAdmission(Job &job)
{
    auto target = job.request.pointRuntime->rasterPointColorizeTarget();
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

LoadJobId RasterColorizeOperation::startColorize(
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
        jobIds_->next("point-cloud colorization job ids are exhausted");
    states_.emplace(jobId,
                    PointCloudColorizeJobState{
                        .jobId = jobId,
                        .pointLayerId = job.request.token.pointLayerId,
                        .rasterLayerId = job.request.token.rasterLayerId,
                        .pointLayerName = job.request.pointLayerName,
                        .rasterLayerName = job.request.rasterLayerName,
                        .phase = PointCloudColorizeJobPhase::Queued,
                        .detail = std::string("Waiting to colorize"),
                        .completion = 0.0,
                    });
    jobs_.emplace(jobId, std::move(job));
    notifyJobStateChanged(jobId);
    schedule(jobId);
    return jobId;
}

void RasterColorizeOperation::schedule(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    Job &job = found->second;
    const AttemptGeneration generation = job.lifecycle.generation;
    const auto callbackTarget = callbackTarget_;
    const auto completionTarget = callbackTarget->reserveCompletion();
    RasterColorizePreflight preflight = job.preflight;
    const PointCloudColorizeRequest request = job.request;
    auto colorReservation = job.colorReservation;
    auto workingReservation = job.workingReservation;
    auto rootReservation = job.rootStagingReservation;
    auto flatReservation = job.flatStagingReservation;
    job.workerCompletion = std::make_shared<WorkerCompletion>();
    const auto workerCompletion = job.workerCompletion;
    const std::stop_token stop = job.lifecycle.stop.get_token();

    job.lifecycle.task = scheduler_->submit(
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
         runStoreFactory = runStoreFactory_,
         workerCompletion,
         stop,
         completionTarget]() mutable {
            struct CompletionGuard {
                std::shared_ptr<WorkerCompletion> completion;
                ~CompletionGuard()
                {
                    completion->finish();
                }
            } completionGuard{workerCompletion};
            std::optional<RasterColorizePreparedPtr> result;
            std::string failure;
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
                    std::move(runStoreFactory),
                    stop,
                    [callbackTarget, jobId, generation](
                        const RasterColorizeProgress value) {
                        postOperationProgress(
                            callbackTarget,
                            {jobId, generation},
                            [jobId, generation, value](
                                RasterColorizeOperation *controller) {
                                controller->observeProgress(
                                    jobId, generation, value);
                            });
                    });
            } catch (const std::exception &error) {
                failure = std::string(error.what());
            }
            post(completionTarget,
                 [jobId,
                  generation,
                  result = std::move(result),
                  failure = std::move(failure)](
                     RasterColorizeOperation *controller) mutable {
                     controller->finishWorker(jobId,
                                              generation,
                                              std::move(result),
                                              std::move(failure));
                 });
        },
        [jobId, generation, workerCompletion, completionTarget] {
            workerCompletion->finish();
            post(completionTarget,
                 [jobId, generation](RasterColorizeOperation *controller) {
                     controller->finishWorker(
                         jobId, generation, std::nullopt, {});
                 });
        },
        job.request.token.pointLayerId.value());
}

void RasterColorizeOperation::observeProgress(
    const LoadJobId jobId,
    const AttemptGeneration generation,
    const RasterColorizeProgress progress)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
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
                 std::string("Reading points and sorting sample locations"),
                 fraction * 0.45);
    } else {
        setState(jobId,
                 PointCloudColorizeJobPhase::SamplingRaster,
                 std::format("Sampling raster tiles ({0} tiles read)",
                             progress.tileReads),
                 0.45 + fraction * 0.5);
    }
}

void RasterColorizeOperation::finishWorker(
    const LoadJobId jobId,
    const AttemptGeneration generation,
    std::optional<RasterColorizePreparedPtr> result,
    std::string failure)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        found->second.lifecycle.finished() ||
        found->second.phase == PointCloudColorizeJobPhase::Committing) {
        return;
    }
    Job &job = found->second;
    job.colorReservation.reset();
    job.workingReservation.reset();
    job.rootStagingReservation.reset();
    job.flatStagingReservation.reset();
    if (job.lifecycle.stop.stop_requested() || (!result && failure.empty())) {
        setState(jobId,
                 PointCloudColorizeJobPhase::Cancelled,
                 std::string("Cancelled"));
        emitTerminal(jobId);
        return;
    }
    if (!failure.empty()) {
        setState(jobId, PointCloudColorizeJobPhase::Failed, failure);
        emitTerminal(jobId, failure);
        return;
    }
    if (!*result) {
        const std::string message =
            std::string("Colorization produced no prepared result");
        setState(jobId, PointCloudColorizeJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }

    RasterColorizePreparedPtr preparedResult = std::move(*result);
    const RasterColorizeStatistics statistics = preparedResult->statistics;
    job.completedStatistics = statistics;
    PointColorInstallationPtr installation;
    try {
        installation = preparePointColorInstallation(std::move(preparedResult));
    } catch (const std::exception &error) {
        const std::string message = std::string(error.what());
        setState(jobId, PointCloudColorizeJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }
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
             std::string("Applying colors"),
             0.95);
    notifyPrepared(
        jobId, job.request.token, std::move(installation), std::move(binding));
}

void RasterColorizeOperation::finishCommit(const LoadJobId jobId,
                                           const bool applied,
                                           std::string failureMessage)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != PointCloudColorizeJobPhase::Committing) {
        return;
    }
    if (!applied) {
        if (failureMessage.empty()) {
            failureMessage = std::string("Layer state changed while colors "
                                         "were being prepared; try again");
        }
        setState(jobId, PointCloudColorizeJobPhase::Failed, failureMessage);
        emitTerminal(jobId, failureMessage);
        return;
    }
    setState(jobId,
             PointCloudColorizeJobPhase::Ready,
             std::format("{0} colored · {1} kept source color",
                         found->second.completedStatistics.pointsColored,
                         found->second.completedStatistics.pointsUnchanged),
             1.0);
    static_cast<void>(found->second.lifecycle.finish());
}

bool RasterColorizeOperation::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != PointCloudColorizeJobPhase::Failed) {
        return false;
    }
    Job &job = found->second;
    job.lifecycle.restart();
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
                 std::string(error.what()));
        return false;
    }
    setState(jobId,
             PointCloudColorizeJobPhase::Queued,
             std::string("Waiting to colorize"),
             0.0);
    schedule(jobId);
    return true;
}

void RasterColorizeOperation::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    found->second.lifecycle.stop.request_stop();
    static_cast<void>(scheduler_->cancel(found->second.lifecycle.task));
}

void RasterColorizeOperation::cancelAll()
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

void RasterColorizeOperation::cancelForPointLayer(
    const PointCloudLayerId layerId)
{
    for (const auto &[id, job] : jobs_) {
        if (job.request.token.pointLayerId == layerId && !terminal(job.phase)) {
            cancel(id);
        }
    }
}

void RasterColorizeOperation::cancelAndWaitForPointLayer(
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

void RasterColorizeOperation::cancelForRasterLayer(const SceneLayerId layerId)
{
    for (const auto &[id, job] : jobs_) {
        if (job.request.token.rasterLayerId == layerId &&
            !terminal(job.phase)) {
            cancel(id);
        }
    }
}

bool RasterColorizeOperation::hasActiveJob(
    const PointCloudLayerId layerId) const noexcept
{
    return std::ranges::any_of(jobs_, [layerId](const auto &entry) {
        return entry.second.request.token.pointLayerId == layerId &&
               !terminal(entry.second.phase);
    });
}

bool RasterColorizeOperation::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

bool RasterColorizeOperation::isCommitInProgress(
    const PointCloudLayerId layerId) const noexcept
{
    return std::ranges::any_of(jobs_, [layerId](const auto &entry) {
        return entry.second.request.token.pointLayerId == layerId &&
               entry.second.phase == PointCloudColorizeJobPhase::Committing;
    });
}

std::optional<PointCloudColorizeJobState>
RasterColorizeOperation::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    return found == states_.end()
               ? std::optional<PointCloudColorizeJobState>{}
               : std::optional<PointCloudColorizeJobState>{found->second};
}

std::vector<PointCloudColorizeJobState>
RasterColorizeOperation::jobStates() const
{
    return sortedJobStates<PointCloudColorizeJobState>(states_);
}

std::vector<OperationRow>
RasterColorizeOperation::jobRows(const std::optional<LoadJobId> only) const
{
    std::vector<OperationRow> rows;
    const auto selected =
        only ? (states_.contains(*only)
                    ? std::vector<PointCloudColorizeJobState>{states_.at(*only)}
                    : std::vector<PointCloudColorizeJobState>{})
             : jobStates();
    for (const PointCloudColorizeJobState &state : selected) {
        const bool isTerminal = terminal(state.phase);
        const std::string target = state.pointLayerName.empty()
                                       ? std::string("point cloud")
                                       : state.pointLayerName;
        const std::string detail = state.rasterLayerName.empty()
                                       ? state.detail
                                       : std::format("From {0} · {1}",
                                                     state.rasterLayerName,
                                                     state.detail);
        rows.push_back({
            .key = {.kind = LoadJobKind::Colorize, .id = state.jobId},
            .title = std::format("Colorize {0}", target),
            .detail = detail,
            .completion = state.completion,
            .terminal = isTerminal,
            .capabilities = isTerminal ? terminalLoadJobCapabilities(
                                             state.phase ==
                                             PointCloudColorizeJobPhase::Failed)
                                       : activeLoadJobCapabilities(),
            .attempt = jobs_.contains(state.jobId)
                           ? jobs_.at(state.jobId).lifecycle.generation
                           : AttemptGeneration{1},
            .state = operationState(state.phase),
            .target = state.pointLayerId,
        });
    }
    return rows;
}

RasterColorizeOperationMetrics RasterColorizeOperation::metrics() const noexcept
{
    RasterColorizeOperationMetrics result;
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

bool RasterColorizeOperation::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !terminal(found->second.phase)) {
        return false;
    }
    jobs_.erase(found);
    states_.erase(jobId);
    notifyJobStateChanged(jobId);
    return true;
}

const TaskScheduler *RasterColorizeOperation::schedulerIdentity() const noexcept
{
    return scheduler_;
}

void RasterColorizeOperation::setState(const LoadJobId jobId,
                                       const PointCloudColorizeJobPhase phase,
                                       std::string detail,
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
    if (!detail.empty()) {
        state->second.detail = std::move(detail);
    }
    if (completion) {
        state->second.completion = *completion;
    }
    notifyJobStateChanged(jobId);
}

void RasterColorizeOperation::emitTerminal(const LoadJobId jobId,
                                           std::string message)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.lifecycle.finished()) {
        return;
    }
    static_cast<void>(found->second.lifecycle.finish());
    if (found->second.phase == PointCloudColorizeJobPhase::Cancelled) {
        notifyCancelled(jobId);
    } else {
        notifyFailed(jobId, std::move(message));
    }
}

} // namespace pci
