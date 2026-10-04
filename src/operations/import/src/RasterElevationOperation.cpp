#include <pci/operations/RasterElevationOperation.h>

#include <pci/operations/OperationTarget.h>

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

[[nodiscard]] bool terminal(const RasterElevationJobPhase phase) noexcept
{
    return phase == RasterElevationJobPhase::Ready ||
           phase == RasterElevationJobPhase::Failed ||
           phase == RasterElevationJobPhase::Cancelled;
}

} // namespace

RasterElevationOperation::RasterElevationOperation(
    TaskScheduler &scheduler, std::shared_ptr<CompletionExecutor> executor)
    : scheduler_(&scheduler)
    , callbackTarget_(
          std::make_shared<OperationTarget<RasterElevationOperation>>(
              this, std::move(executor)))
{
}

RasterElevationOperation::~RasterElevationOperation()
{
    events_ = {};
    registration_.detach();
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId
RasterElevationOperation::start(const RasterElevationBindingToken token,
                                RasterTileSourcePtr source)
{
    if (token.layerId.value() == 0 || token.sourceId.value() == 0 ||
        token.bindingGeneration.value() == 0 || !source ||
        !source->metadata().elevation.available) {
        throw std::invalid_argument(
            "elevation analysis requires an eligible raster source");
    }
    if (const auto existing = layerJobs_.find(token.layerId);
        existing != layerJobs_.end()) {
        const auto state = states_.find(existing->second);
        if (state != states_.end() && !terminal(state->second.phase)) {
            return existing->second;
        }
    }

    const LoadJobId jobId =
        jobIds_->next("raster elevation job ids are exhausted");
    jobs_.emplace(jobId,
                  Job{
                      .token = token,
                      .source = std::move(source),
                      .lifecycle = {},
                      .phase = RasterElevationJobPhase::Queued,
                  });
    states_.emplace(jobId,
                    RasterElevationJobState{
                        .jobId = jobId,
                        .layerId = token.layerId,
                        .sourceId = token.sourceId,
                        .bindingGeneration = token.bindingGeneration,
                        .detail = {},
                    });
    layerJobs_.insert_or_assign(token.layerId, jobId);
    pending_.push_back(jobId);
    notifyJobStateChanged(jobId);
    scheduleNext();
    return jobId;
}

void RasterElevationOperation::scheduleNext()
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
        notifyJobStateChanged(jobId);

        const AttemptGeneration generation = job.lifecycle.generation;
        const RasterTileSourcePtr source = job.source;
        const std::stop_token stop = job.lifecycle.stop.get_token();
        const auto callbackTarget = callbackTarget_;
        const auto completionTarget = callbackTarget->reserveCompletion();
        const auto cancelledCallback = [jobId, generation, completionTarget] {
            post(completionTarget,
                 [jobId, generation](RasterElevationOperation *controller) {
                     controller->finish(
                         jobId, generation, std::nullopt, {}, true);
                 });
        };
        job.lifecycle.task = scheduler_->submit(
            TaskPriority::Background,
            source->exactElevationScanReservationBytes(),
            [callbackTarget,
             jobId,
             generation,
             source,
             stop,
             completionTarget] {
                try {
                    const RasterElevationRange range =
                        source->exactElevationRange(
                            stop,
                            [callbackTarget, jobId, generation](
                                const RasterElevationScanProgress progress) {
                                postOperationProgress(
                                    callbackTarget,
                                    {jobId, generation},
                                    [jobId, generation, progress](
                                        RasterElevationOperation *controller) {
                                        controller->observeProgress(
                                            jobId, generation, progress);
                                    });
                            });
                    post(completionTarget,
                         [jobId, generation, range](
                             RasterElevationOperation *controller) {
                             controller->finish(
                                 jobId, generation, range, {}, false);
                         });
                } catch (const RasterReadCancelled &) {
                    post(completionTarget,
                         [jobId,
                          generation](RasterElevationOperation *controller) {
                             controller->finish(
                                 jobId, generation, std::nullopt, {}, true);
                         });
                } catch (const std::exception &error) {
                    const std::string message = std::string(error.what());
                    post(completionTarget,
                         [jobId, generation, message](
                             RasterElevationOperation *controller) {
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

void RasterElevationOperation::observeProgress(
    const LoadJobId jobId,
    const AttemptGeneration generation,
    const RasterElevationScanProgress progress)
{
    const auto job = jobs_.find(jobId);
    const auto state = states_.find(jobId);
    if (job == jobs_.end() || state == states_.end() ||
        job->second.lifecycle.generation != generation ||
        job->second.lifecycle.finished()) {
        return;
    }
    state->second.processedBlocks = progress.processedBlocks;
    state->second.totalBlocks = progress.totalBlocks;
    notifyJobStateChanged(jobId);
}

void RasterElevationOperation::finish(
    const LoadJobId jobId,
    const AttemptGeneration generation,
    const std::optional<RasterElevationRange> range,
    std::string error,
    const bool wasCancelled)
{
    const auto job = jobs_.find(jobId);
    const auto state = states_.find(jobId);
    if (job == jobs_.end() || state == states_.end() ||
        job->second.lifecycle.generation != generation ||
        !job->second.lifecycle.finish()) {
        return;
    }
    job->second.lifecycle.task = {};
    if (active_ == jobId) {
        active_.reset();
    }
    if (wasCancelled) {
        job->second.phase = RasterElevationJobPhase::Cancelled;
        state->second.phase = RasterElevationJobPhase::Cancelled;
        notifyCancelled(jobId, job->second.token);
    } else if (range) {
        job->second.phase = RasterElevationJobPhase::Ready;
        state->second.phase = RasterElevationJobPhase::Ready;
        state->second.processedBlocks = state->second.totalBlocks;
        notifyCompleted(jobId, job->second.token, *range);
    } else {
        job->second.phase = RasterElevationJobPhase::Failed;
        state->second.phase = RasterElevationJobPhase::Failed;
        state->second.detail = std::move(error);
        notifyFailed(jobId, job->second.token, state->second.detail);
    }
    notifyJobStateChanged(jobId);
    scheduleNext();
}

void RasterElevationOperation::cancelLayer(const SceneLayerId layerId)
{
    if (const auto found = layerJobs_.find(layerId);
        found != layerJobs_.end()) {
        cancel(found->second);
    }
}

void RasterElevationOperation::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    found->second.lifecycle.stop.request_stop();
    if (found->second.lifecycle.task != TaskId{} &&
        scheduler_->cancel(found->second.lifecycle.task)) {
        found->second.lifecycle.task = {};
        return;
    }
    if (found->second.phase == RasterElevationJobPhase::Queued) {
        finish(
            jobId, found->second.lifecycle.generation, std::nullopt, {}, true);
    }
}

void RasterElevationOperation::cancelAll()
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

bool RasterElevationOperation::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        (found->second.phase != RasterElevationJobPhase::Failed &&
         found->second.phase != RasterElevationJobPhase::Cancelled)) {
        return false;
    }
    Job &job = found->second;
    job.lifecycle.restart();
    job.phase = RasterElevationJobPhase::Queued;
    RasterElevationJobState &state = states_.at(jobId);
    state.phase = RasterElevationJobPhase::Queued;
    state.processedBlocks = 0;
    state.totalBlocks = 0;
    state.detail.clear();
    pending_.push_back(jobId);
    notifyJobStateChanged(jobId);
    scheduleNext();
    return true;
}

bool RasterElevationOperation::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !terminal(found->second.phase)) {
        return false;
    }
    layerJobs_.erase(found->second.token.layerId);
    jobs_.erase(found);
    states_.erase(jobId);
    notifyJobStateChanged(jobId);
    return true;
}

bool RasterElevationOperation::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

std::vector<RasterElevationJobState> RasterElevationOperation::jobStates() const
{
    std::vector<RasterElevationJobState> result;
    result.reserve(states_.size());
    for (const auto &[id, state] : states_) {
        result.push_back(state);
    }
    std::ranges::sort(result, {}, &RasterElevationJobState::jobId);
    return result;
}

std::vector<OperationRow>
RasterElevationOperation::jobRows(const std::optional<LoadJobId> only) const
{
    std::vector<OperationRow> rows;
    const auto selected =
        only ? (states_.contains(*only)
                    ? std::vector<RasterElevationJobState>{states_.at(*only)}
                    : std::vector<RasterElevationJobState>{})
             : jobStates();
    for (const RasterElevationJobState &state : selected) {
        std::string title = std::string("DEM elevation analysis");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path filename =
                job->second.source->metadata().sourcePath.filename();
            if (!filename.empty()) {
                title = operationPath(filename);
            }
        }
        std::string detail = state.detail;
        switch (state.phase) {
        case RasterElevationJobPhase::Queued:
            detail = std::string("Queued for exact elevation analysis");
            break;
        case RasterElevationJobPhase::Scanning:
            detail = std::string("Scanning exact elevation range");
            break;
        case RasterElevationJobPhase::Ready:
            detail = std::string("Elevation range ready");
            break;
        case RasterElevationJobPhase::Failed:
            break;
        case RasterElevationJobPhase::Cancelled:
            detail = std::string("Cancelled");
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
                isTerminal ? terminalLoadJobCapabilities(
                                 state.phase == RasterElevationJobPhase::Failed)
                           : activeLoadJobCapabilities(),
            .attempt = jobs_.contains(state.jobId)
                           ? jobs_.at(state.jobId).lifecycle.generation
                           : AttemptGeneration{1},
            .state = operationState(state.phase),
            .target = state.layerId,
        });
    }
    return rows;
}

const TaskScheduler *
RasterElevationOperation::schedulerIdentity() const noexcept
{
    return scheduler_;
}

} // namespace pci
