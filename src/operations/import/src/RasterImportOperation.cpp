#include <pci/operations/RasterImportOperation.h>

#include <pci/operations/OperationTarget.h>

#include <algorithm>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

// Inspection reads headers and, at most, a bounded window sample. It never
// reads a full-resolution payload, so its reservation stays small.
constexpr std::uint64_t inspectionReservationBytes = std::uint64_t{1024} * 1024;

template <typename Target, typename Completion>
void post(const std::shared_ptr<Target> &target, Completion completion)
{
    postToOperation(target, std::move(completion));
}

JobError rasterCancelledError()
{
    return {
        .code = JobErrorCode::Cancelled,
        .message = "Raster import cancelled",
    };
}

[[nodiscard]] bool terminal(const RasterLoadJobPhase phase) noexcept
{
    return phase == RasterLoadJobPhase::Ready ||
           phase == RasterLoadJobPhase::Failed ||
           phase == RasterLoadJobPhase::Cancelled;
}

} // namespace

RasterImportOperation::RasterImportOperation(
    std::shared_ptr<const RasterLoader> loader,
    TaskScheduler &scheduler,
    std::shared_ptr<CompletionExecutor> executor)
    : loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("raster loader must not be null");
    }
    callbackTarget_ = std::make_shared<OperationTarget<RasterImportOperation>>(
        this, std::move(executor));
}

RasterImportOperation::~RasterImportOperation()
{
    events_ = {};
    registration_.detach();
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId RasterImportOperation::createJob(RasterImportRequest request)
{
    const LoadJobId jobId = jobIds_->next("raster job ids are exhausted");
    Job job;
    job.request = std::move(request);
    job.request.stopToken = job.lifecycle.stop.get_token();
    job.phase = RasterLoadJobPhase::Inspecting;
    jobs_.emplace(jobId, std::move(job));
    states_.emplace(jobId,
                    RasterLoadJobState{
                        .jobId = jobId,
                        .phase = RasterLoadJobPhase::Inspecting,
                        .detail = {},
                    });
    notifyJobStateChanged(jobId);
    return jobId;
}

LoadJobId RasterImportOperation::startImport(RasterImportRequest request)
{
    if (request.sourcePath.empty()) {
        throw std::invalid_argument("raster import requires a source path");
    }
    const LoadJobId jobId = createJob(std::move(request));
    scheduleInspection(jobId);
    return jobId;
}

void RasterImportOperation::scheduleInspection(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    const AttemptGeneration generation = found->second.lifecycle.generation;
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    const auto completionTarget = callbackTarget->reserveCompletion();

    RasterImportRequest request = found->second.request;
    // The worker reports its phase back through the same queued callback the
    // result uses, so the UI never reads worker-thread state directly.
    request.phase =
        [callbackTarget, jobId, generation](const RasterImportPhase phase) {
            postOperationProgress(
                callbackTarget,
                {jobId, generation},
                [jobId, generation, phase](RasterImportOperation *controller) {
                    controller->observePhase(jobId, generation, phase);
                });
        };

    const auto completeCancelled = [jobId, generation, completionTarget] {
        post(
            completionTarget,
            [jobId, generation](RasterImportOperation *controller) {
                controller->finishInspection(
                    jobId, generation, std::unexpected(rasterCancelledError()));
            });
    };

    found->second.lifecycle.tasks.push_back(scheduler_->submit(
        TaskPriority::Inspection,
        inspectionReservationBytes,
        [jobId, generation, loader, request, completionTarget] {
            InspectionResult result = [&]() -> InspectionResult {
                try {
                    return loader->inspect(request);
                } catch (const RasterImportCancelled &) {
                    return std::unexpected(rasterCancelledError());
                } catch (const RasterImportError &error) {
                    return std::unexpected(JobError{
                        .code = JobErrorCode::Io,
                        .message = error.what(),
                    });
                } catch (const std::exception &error) {
                    return std::unexpected(JobError{
                        .code = JobErrorCode::Internal,
                        .message = error.what(),
                    });
                }
            }();
            post(completionTarget,
                 [jobId, generation, result = std::move(result)](
                     RasterImportOperation *controller) mutable {
                     controller->finishInspection(
                         jobId, generation, std::move(result));
                 });
        },
        completeCancelled,
        jobId.value()));
}

void RasterImportOperation::observePhase(const LoadJobId jobId,
                                         const AttemptGeneration generation,
                                         const RasterImportPhase phase)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        terminal(found->second.phase)) {
        return;
    }
    setState(jobId,
             phase == RasterImportPhase::SamplingRange
                 ? RasterLoadJobPhase::SamplingRange
                 : RasterLoadJobPhase::Inspecting);
}

void RasterImportOperation::finishInspection(const LoadJobId jobId,
                                             const AttemptGeneration generation,
                                             InspectionResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation)
        return;
    Job &job = found->second;
    if (job.lifecycle.finished()) {
        return;
    }
    if (job.cancelled ||
        (!result && result.error().code == JobErrorCode::Cancelled)) {
        setState(jobId, RasterLoadJobPhase::Cancelled);
        emitTerminal(jobId, {});
        return;
    }
    if (!result) {
        const std::string message = std::string(result.error().message);
        setState(jobId, RasterLoadJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }
    if (!result->data || !result->data->source) {
        const std::string message =
            std::string("Raster inspection produced no source");
        setState(jobId, RasterLoadJobPhase::Failed, message);
        emitTerminal(jobId, message);
        return;
    }

    const RasterLayerMetadata &metadata = result->data->metadata();
    // A raster that shares no XY extent with the scene starts hidden, reusing
    // the "Show anyway" affordance vector layers already provide.
    const bool initiallyVisible = !metadata.extentDisjointXY;
    JobResult<void> installed;
    try {
        if (installer_) {
            installed = installer_(jobId,
                                   job.request.session,
                                   generation,
                                   result->data,
                                   initiallyVisible);
        }
    } catch (const std::exception &error) {
        installed =
            std::unexpected(JobError{JobErrorCode::Internal, error.what()});
    }
    // Installation can synchronously reset/cancel the session.
    const auto current = jobs_.find(jobId);
    if (current == jobs_.end() ||
        current->second.lifecycle.generation != generation ||
        current->second.lifecycle.finished()) {
        return;
    }
    if (current->second.cancelled ||
        (!installed && installed.error().code == JobErrorCode::Cancelled)) {
        setState(jobId, RasterLoadJobPhase::Cancelled);
        emitTerminal(jobId, {});
    } else if (!installed) {
        const auto message = std::string(installed.error().message);
        setState(jobId, RasterLoadJobPhase::Failed, message);
        emitTerminal(jobId, message);
    } else {
        static_cast<void>(current->second.lifecycle.finish());
        setState(jobId, RasterLoadJobPhase::Ready);
        notifyLoaded(jobId, result->data, initiallyVisible);
    }
}

bool RasterImportOperation::retry(const LoadJobId jobId,
                                  const SessionGeneration session)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != RasterLoadJobPhase::Failed) {
        return false;
    }
    Job &job = found->second;
    job.lifecycle.restart();
    if (session.value()) {
        job.request.session = session;
    }
    job.cancelled = false;
    job.request.stopToken = job.lifecycle.stop.get_token();
    setState(jobId, RasterLoadJobPhase::Inspecting);
    scheduleInspection(jobId);
    return true;
}

void RasterImportOperation::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || terminal(found->second.phase)) {
        return;
    }
    Job &job = found->second;
    job.cancelled = true;
    job.lifecycle.stop.request_stop();
    for (const TaskScheduler::TaskId task : job.lifecycle.tasks) {
        static_cast<void>(scheduler_->cancel(task));
    }
}

void RasterImportOperation::cancelAll()
{
    std::vector<LoadJobId> active;
    active.reserve(jobs_.size());
    for (const auto &[jobId, job] : jobs_) {
        if (!terminal(job.phase)) {
            active.push_back(jobId);
        }
    }
    for (const LoadJobId jobId : active) {
        cancel(jobId);
    }
}

bool RasterImportOperation::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !terminal(entry.second.phase);
    });
}

std::optional<RasterLoadJobState>
RasterImportOperation::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<RasterLoadJobState> RasterImportOperation::jobStates() const
{
    return sortedJobStates<RasterLoadJobState>(states_);
}

std::vector<OperationRow>
RasterImportOperation::jobRows(const std::optional<LoadJobId> only) const
{
    std::vector<OperationRow> rows;
    rows.reserve(states_.size());
    const auto selected =
        only ? (states_.contains(*only)
                    ? std::vector<RasterLoadJobState>{states_.at(*only)}
                    : std::vector<RasterLoadJobState>{})
             : jobStates();
    for (const RasterLoadJobState &state : selected) {
        std::string title = std::string("Raster import");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path source =
                job->second.request.sourcePath.filename();
            if (!source.empty()) {
                title = operationPath(source);
            }
        }

        std::string detail = state.detail;
        double completion = 0.0;
        switch (state.phase) {
        case RasterLoadJobPhase::Queued:
            detail = std::string("Queued");
            break;
        case RasterLoadJobPhase::Inspecting:
            detail = std::string("Reading raster metadata");
            break;
        case RasterLoadJobPhase::SamplingRange:
            detail = std::string("Sampling display range");
            completion = 0.5;
            break;
        case RasterLoadJobPhase::Ready:
            detail = std::string("Loaded");
            completion = 1.0;
            break;
        case RasterLoadJobPhase::Failed:
            break;
        case RasterLoadJobPhase::Cancelled:
            detail = std::string("Cancelled");
            break;
        }

        const bool isTerminal = terminal(state.phase);
        rows.push_back(OperationRow{
            .key = {.kind = LoadJobKind::Raster, .id = state.jobId},
            .title = std::move(title),
            .detail = std::move(detail),
            .completion = completion,
            .terminal = isTerminal,
            .capabilities = isTerminal
                                ? terminalLoadJobCapabilities(
                                      state.phase == RasterLoadJobPhase::Failed)
                                : activeLoadJobCapabilities(),
            .attempt = jobs_.contains(state.jobId)
                           ? jobs_.at(state.jobId).lifecycle.generation
                           : AttemptGeneration{1},
            .state = operationState(state.phase),
            .target = std::nullopt,
        });
    }
    return rows;
}

bool RasterImportOperation::dismiss(const LoadJobId jobId)
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

const TaskScheduler *RasterImportOperation::schedulerIdentity() const noexcept
{
    return scheduler_;
}

void RasterImportOperation::setState(const LoadJobId jobId,
                                     const RasterLoadJobPhase phase,
                                     std::string detail)
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
    notifyJobStateChanged(jobId);
}

void RasterImportOperation::emitTerminal(const LoadJobId jobId,
                                         std::string message)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.lifecycle.finished()) {
        return;
    }
    static_cast<void>(found->second.lifecycle.finish());
    if (found->second.phase == RasterLoadJobPhase::Cancelled) {
        notifyCancelled(jobId);
    } else {
        notifyFailed(jobId, std::move(message));
    }
}

} // namespace pci
