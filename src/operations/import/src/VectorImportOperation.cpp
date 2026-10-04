#include <pci/operations/VectorImportOperation.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/operations/OperationTarget.h>

#include <algorithm>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace pci {
namespace {

constexpr std::uint64_t inspectionReservationBytes = std::uint64_t{1024} * 1024;

template <typename Target, typename Completion>
void post(const std::shared_ptr<Target> &target, Completion completion)
{
    postToOperation(target, std::move(completion));
}

bool contains(const std::vector<VectorSublayerKey> &keys,
              const VectorSublayerKey &key)
{
    return std::ranges::find(keys, key) != keys.end();
}

JobError vectorCancelledError()
{
    return {
        .code = JobErrorCode::Cancelled,
        .message = "Vector import cancelled",
    };
}

std::string jobErrorMessage(const JobError &error)
{
    return std::string(error.message);
}

} // namespace

VectorImportOperation::VectorImportOperation(
    std::shared_ptr<const VectorLoader> loader,
    TaskScheduler &scheduler,
    std::shared_ptr<CompletionExecutor> executor)
    : loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("vector loader must not be null");
    }
    callbackTarget_ = std::make_shared<OperationTarget<VectorImportOperation>>(
        this, std::move(executor));
}

VectorImportOperation::~VectorImportOperation()
{
    events_ = {};
    registration_.detach();
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId VectorImportOperation::createJob(VectorImportRequest request,
                                           const VectorLoadJobPhase phase)
{
    const LoadJobId jobId = jobIds_->next("vector job ids are exhausted");
    Job job;
    job.request = std::move(request);
    job.request.stopToken = job.lifecycle.stop.get_token();
    job.phase = phase;
    jobs_.emplace(jobId, std::move(job));
    states_.emplace(jobId,
                    VectorLoadJobState{
                        .jobId = jobId,
                        .phase = phase,
                        .summary = {},
                        .detail = {},
                    });
    notifyJobStateChanged(jobId);
    return jobId;
}

LoadJobId VectorImportOperation::startInspection(VectorImportRequest request)
{
    if (scheduler_->activeByteBudget() < inspectionReservationBytes ||
        request.limits.maximumApplicationWorkingBytes == 0 ||
        request.limits.maximumApplicationWorkingBytes >
            scheduler_->activeByteBudget()) {
        throw std::invalid_argument(
            "vector working-byte limit is not schedulable");
    }
    const auto jobId =
        createJob(std::move(request), VectorLoadJobPhase::Inspecting);
    scheduleInspection(jobId);
    return jobId;
}

void VectorImportOperation::scheduleInspection(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    const AttemptGeneration generation = found->second.lifecycle.generation;
    const VectorImportRequest request = found->second.request;
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    const auto completionTarget = callbackTarget->reserveCompletion();
    const auto completeCancelled = [jobId, generation, completionTarget] {
        post(
            completionTarget,
            [jobId, generation](VectorImportOperation *controller) {
                controller->finishInspection(
                    jobId, generation, std::unexpected(vectorCancelledError()));
            });
    };
    found->second.lifecycle.tasks.push_back(scheduler_->submit(
        TaskPriority::Inspection,
        inspectionReservationBytes,
        [jobId, generation, loader, request, completionTarget] {
            InspectionResult result = [&]() -> InspectionResult {
                try {
                    return loader->inspect(request);
                } catch (const VectorImportCancelled &) {
                    return std::unexpected(vectorCancelledError());
                } catch (const VectorImportError &error) {
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
                     VectorImportOperation *controller) mutable {
                     controller->finishInspection(
                         jobId, generation, std::move(result));
                 });
        },
        completeCancelled,
        jobId.value()));
}

void VectorImportOperation::finishInspection(const LoadJobId jobId,
                                             const AttemptGeneration generation,
                                             InspectionResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        found->second.phase != VectorLoadJobPhase::Inspecting ||
        found->second.lifecycle.finished())
        return;
    Job &job = found->second;
    if (job.cancelled ||
        (!result && result.error().code == JobErrorCode::Cancelled)) {
        emitTerminal(jobId);
        return;
    }
    if (!result) {
        const std::string message = jobErrorMessage(result.error());
        states_.at(jobId).detail = message;
        setState(jobId, VectorLoadJobPhase::Failed, message);
        emitTerminal(jobId);
        return;
    }
    if (result->sublayers.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Failed,
                 std::string("No vector sublayers were found"));
        emitTerminal(jobId);
        return;
    }
    job.preflight = std::move(*result);
    setState(jobId, VectorLoadJobPhase::AwaitingChoice);
    notifyInspected(jobId, *job.preflight);
}

bool VectorImportOperation::continueLoad(
    const LoadJobId jobId, std::vector<VectorSublayerKey> selected)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.phase != VectorLoadJobPhase::AwaitingChoice ||
        selected.empty())
        return false;
    Job &job = found->second;
    job.request.sublayers = std::move(selected);
    job.summary.selected = job.request.sublayers;
    states_.at(jobId).summary = job.summary;
    beginLoad(jobId);
    return true;
}

void VectorImportOperation::beginLoad(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    Job &job = found->second;
    if (job.request.sublayers.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Failed,
                 std::string("No vector sublayers were selected"));
        emitTerminal(jobId);
        return;
    }
    if (!job.request.origin && job.preflight) {
        Bounds3d unionBounds;
        bool haveBounds = false;
        bool allReported = true;
        for (const VectorSublayerKey &key : job.request.sublayers) {
            const auto info = std::ranges::find(
                job.preflight->sublayers, key, &VectorSublayerInfo::key);
            if (info == job.preflight->sublayers.end() || !info->extent) {
                allReported = false;
                break;
            }
            if (!haveBounds) {
                unionBounds = *info->extent;
                haveBounds = true;
            } else {
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    unionBounds.minimum[axis] = std::min(
                        unionBounds.minimum[axis], info->extent->minimum[axis]);
                    unionBounds.maximum[axis] = std::max(
                        unionBounds.maximum[axis], info->extent->maximum[axis]);
                }
            }
        }
        if (allReported && haveBounds) {
            job.request.origin = {std::array<double, 2>{
                (unionBounds.minimum[0] + unionBounds.maximum[0]) * 0.5,
                (unionBounds.minimum[1] + unionBounds.maximum[1]) * 0.5}};
        }
    }
    if (!job.request.origin) {
        setState(jobId, VectorLoadJobPhase::ResolvingOrigin);
        const AttemptGeneration generation = job.lifecycle.generation;
        const VectorImportRequest request = job.request;
        const auto loader = loader_;
        const auto callbackTarget = callbackTarget_;
        const auto completionTarget = callbackTarget->reserveCompletion();
        const auto cancelled = [jobId, generation, completionTarget] {
            post(completionTarget,
                 [jobId, generation](VectorImportOperation *controller) {
                     controller->finishOrigin(
                         jobId,
                         generation,
                         std::unexpected(vectorCancelledError()));
                 });
        };
        job.lifecycle.tasks.push_back(scheduler_->submit(
            TaskPriority::Inspection,
            inspectionReservationBytes,
            [jobId, generation, request, loader, completionTarget] {
                OriginResult result = [&]() -> OriginResult {
                    try {
                        return loader->probeOrigin(request, request.sublayers);
                    } catch (const VectorImportCancelled &) {
                        return std::unexpected(vectorCancelledError());
                    } catch (const VectorImportError &error) {
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
                         VectorImportOperation *controller) mutable {
                         controller->finishOrigin(
                             jobId, generation, std::move(result));
                     });
            },
            cancelled,
            jobId.value()));
        return;
    }
    setState(jobId, VectorLoadJobPhase::Reading);
    job.outstanding = 0;
    job.nextSublayer = 0;
    job.received.assign(job.request.sublayers.size(), false);
    job.sublayerProgress.assign(job.request.sublayers.size(), {});
    VectorLoadJobState &state = states_.at(jobId);
    state.processed = 0;
    state.total = 0;
    scheduleSublayers(jobId);
}

void VectorImportOperation::scheduleSublayers(const LoadJobId jobId)
{
    auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    auto &job = found->second;
    const auto generation = job.lifecycle.generation;
    while (!job.cancelled && job.nextSublayer < job.request.sublayers.size() &&
           job.outstanding < scheduler_->maximumWorkers()) {
        const auto key = job.request.sublayers[job.nextSublayer++];
        ++job.outstanding;
        VectorImportRequest request = job.request;
        const auto loader = loader_;
        const auto callbackTarget = callbackTarget_;
        const auto completionTarget = callbackTarget->reserveCompletion();
        const auto cancelled = [jobId, key, generation, completionTarget] {
            post(completionTarget,
                 [jobId, key, generation](VectorImportOperation *controller) {
                     controller->finishOne(
                         jobId,
                         key,
                         generation,
                         std::unexpected(vectorCancelledError()));
                 });
        };
        struct ProgressMailbox {
            std::mutex mutex;
            VectorImportProgress latest;
            bool pending = false;
        };
        auto mailbox = std::make_shared<ProgressMailbox>();
        const auto downstreamProgress = request.progress;
        request.progress = [callbackTarget,
                            jobId,
                            key,
                            generation,
                            downstreamProgress,
                            mailbox](const VectorImportProgress progress) {
            if (downstreamProgress) {
                downstreamProgress(progress);
            }
            {
                const std::scoped_lock lock(mailbox->mutex);
                mailbox->latest = progress;
                if (mailbox->pending) {
                    return;
                }
                mailbox->pending = true;
            }
            bool accepted = false;
            try {
                accepted = postToOperation(
                    callbackTarget,
                    [jobId, key, generation, mailbox](
                        VectorImportOperation *controller) {
                        VectorImportProgress latest;
                        {
                            const std::scoped_lock lock(mailbox->mutex);
                            latest = mailbox->latest;
                            mailbox->pending = false;
                        }
                        controller->reportProgress(
                            jobId, key, generation, latest);
                    });
            } catch (...) {
            }
            if (!accepted) {
                const std::scoped_lock lock(mailbox->mutex);
                mailbox->pending = false;
            }
        };
        const std::uint64_t estimatedBytes =
            request.limits.maximumApplicationWorkingBytes;
        job.lifecycle.tasks.push_back(scheduler_->submit(
            TaskPriority::Import,
            estimatedBytes,
            [jobId,
             key,
             generation,
             request = std::move(request),
             loader,
             completionTarget] {
                SublayerResult result = [&]() -> SublayerResult {
                    try {
                        VectorLayerDataPtr data =
                            loader->loadSublayer(request, key);
                        if (!data) {
                            return std::unexpected(JobError{
                                .code = JobErrorCode::Internal,
                                .message =
                                    "Vector loader returned no layer data",
                            });
                        }
                        if (data->retainedBytes() >
                            request.limits.maximumRetainedBytes) {
                            throw VectorImportLimitExceeded(
                                "prepared vector retained bytes",
                                data->retainedBytes());
                        }
                        return data;
                    } catch (const VectorImportCancelled &) {
                        return std::unexpected(vectorCancelledError());
                    } catch (const VectorImportLimitExceeded &error) {
                        return std::unexpected(JobError{
                            .code = JobErrorCode::ResourceAdmission,
                            .message = error.what(),
                        });
                    } catch (const VectorImportError &error) {
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
                     [jobId, key, generation, result = std::move(result)](
                         VectorImportOperation *controller) mutable {
                         controller->finishOne(
                             jobId, key, generation, std::move(result));
                     });
            },
            cancelled,
            jobId.value()));
    }
}

void VectorImportOperation::reportProgress(const LoadJobId jobId,
                                           const VectorSublayerKey &key,
                                           const AttemptGeneration generation,
                                           const VectorImportProgress progress)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        found->second.cancelled || found->second.lifecycle.finished()) {
        return;
    }
    Job &job = found->second;
    const auto keyIt = std::ranges::find(job.request.sublayers, key);
    if (keyIt == job.request.sublayers.end())
        return;
    const std::size_t index = static_cast<std::size_t>(
        std::distance(job.request.sublayers.begin(), keyIt));
    if (index >= job.sublayerProgress.size())
        return;

    VectorImportProgress &previous = job.sublayerProgress[index];
    previous.processed = std::max(previous.processed, progress.processed);
    // A loader may initially report an unknown total and discover it later.
    // Do not replace a known total with zero or allow it to move backwards.
    previous.total = progress.total == 0
                         ? previous.total
                         : std::max(previous.total, progress.total);

    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    bool allTotalsKnown = !job.sublayerProgress.empty();
    for (const VectorImportProgress value : job.sublayerProgress) {
        processed = saturatingAdd(processed, value.processed);
        if (value.total == 0) {
            allTotalsKnown = false;
            continue;
        }
        total = saturatingAdd(total, value.total);
    }
    if (!allTotalsKnown)
        total = 0;
    VectorLoadJobState &state = states_.at(jobId);
    if (processed == state.processed && total == state.total)
        return;
    state.processed = std::max(state.processed, processed);
    state.total = total == 0 ? 0 : std::max(state.total, total);
    notifyProgressChanged(jobId, state.processed, state.total);
    notifyJobStateChanged(jobId);
}

void VectorImportOperation::finishOrigin(const LoadJobId jobId,
                                         const AttemptGeneration generation,
                                         OriginResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        found->second.phase != VectorLoadJobPhase::ResolvingOrigin ||
        found->second.lifecycle.finished())
        return;
    if (found->second.cancelled ||
        (!result && result.error().code == JobErrorCode::Cancelled)) {
        emitTerminal(jobId);
        return;
    }
    if (!result) {
        setState(
            jobId, VectorLoadJobPhase::Failed, jobErrorMessage(result.error()));
        emitTerminal(jobId);
        return;
    }
    found->second.request.origin = *result;
    beginLoad(jobId);
}

void VectorImportOperation::finishOne(const LoadJobId jobId,
                                      VectorSublayerKey key,
                                      const AttemptGeneration generation,
                                      SublayerResult result)
{
    auto found = jobs_.find(jobId);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != generation ||
        found->second.lifecycle.finished() ||
        found->second.phase != VectorLoadJobPhase::Reading) {
        return;
    }
    auto &job = found->second;
    const auto position = std::ranges::find(job.request.sublayers, key);
    if (position == job.request.sublayers.end()) {
        return;
    }
    const auto index =
        static_cast<std::size_t>(position - job.request.sublayers.begin());
    if (job.received[index]) {
        return;
    }
    job.received[index] = true;
    --job.outstanding;
    const bool wasCancelled =
        !result && result.error().code == JobErrorCode::Cancelled;
    if (!job.cancelled && !wasCancelled) {
        if (result && !contains(job.summary.successful, key)) {
            // Allocate the eventual summary before the owner commits any data.
            VectorLoadSummary preparedSummary;
            VectorLoadSummary preparedStateSummary;
            JobResult<void> installed;
            try {
                preparedSummary = job.summary;
                preparedSummary.successful.push_back(key);
                preparedStateSummary = preparedSummary;
                if (installer_) {
                    installed = installer_(
                        jobId, job.request.session, generation, key, *result);
                }
            } catch (const std::exception &error) {
                installed = std::unexpected(
                    JobError{JobErrorCode::Internal, error.what()});
            }
            found = jobs_.find(jobId);
            if (found == jobs_.end() ||
                found->second.lifecycle.generation != generation ||
                found->second.lifecycle.finished()) {
                return;
            }
            if (found->second.cancelled) {
                result = std::unexpected(vectorCancelledError());
            } else if (!installed) {
                result = std::unexpected(installed.error());
            } else {
                std::swap(found->second.summary, preparedSummary);
                std::swap(states_.at(jobId).summary, preparedStateSummary);
                notifySublayerLoaded(jobId, key, *result);
            }
        }
        found = jobs_.find(jobId);
        if (found == jobs_.end() ||
            found->second.lifecycle.generation != generation) {
            return;
        }
        if (!result) {
            if (result.error().code == JobErrorCode::Cancelled) {
                found->second.cancelled = true;
            } else {
                found->second.summary.failed.push_back(
                    {key, result.error().message});
                notifySublayerFailed(
                    jobId, key, jobErrorMessage(result.error()));
            }
        }
        if (!result) {
            states_.at(jobId).summary = found->second.summary;
        }
    }
    scheduleSublayers(jobId);
    found = jobs_.find(jobId);
    if (found != jobs_.end() &&
        found->second.lifecycle.generation == generation &&
        found->second.outstanding == 0) {
        emitTerminal(jobId);
    }
}

void VectorImportOperation::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.lifecycle.finished())
        return;
    Job &job = found->second;
    job.cancelled = true;
    job.lifecycle.stop.request_stop();
    for (const auto task : job.lifecycle.tasks)
        static_cast<void>(scheduler_->cancel(task));
    if (job.phase != VectorLoadJobPhase::Reading &&
        job.phase != VectorLoadJobPhase::ResolvingOrigin &&
        job.phase != VectorLoadJobPhase::Inspecting)
        emitTerminal(jobId);
}

void VectorImportOperation::cancelAll()
{
    std::vector<LoadJobId> ids;
    ids.reserve(jobs_.size());
    for (const auto &[id, job] : jobs_) {
        static_cast<void>(job);
        ids.push_back(id);
    }
    for (const auto id : ids)
        cancel(id);
}

bool VectorImportOperation::retry(const LoadJobId jobId,
                                  const SessionGeneration session)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !found->second.lifecycle.finished())
        return false;
    Job &job = found->second;
    std::vector<VectorSublayerKey> retryKeys;
    for (const VectorSublayerKey &key : job.summary.selected) {
        if (!contains(job.summary.successful, key))
            retryKeys.push_back(key);
    }
    if (retryKeys.empty())
        return false;
    job.lifecycle.restart();
    if (session.value()) {
        job.request.session = session;
    }
    job.request.stopToken = job.lifecycle.stop.get_token();
    job.request.sublayers = std::move(retryKeys);
    job.summary.failed.clear();
    job.outstanding = 0;
    job.cancelled = false;
    beginLoad(jobId);
    return true;
}

bool VectorImportOperation::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !entry.second.lifecycle.finished();
    });
}

std::optional<VectorLoadJobState>
VectorImportOperation::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    return found == states_.end()
               ? std::nullopt
               : std::optional<VectorLoadJobState>(found->second);
}

std::vector<VectorLoadJobState> VectorImportOperation::jobStates() const
{
    return sortedJobStates<VectorLoadJobState>(states_);
}

std::vector<OperationRow>
VectorImportOperation::jobRows(const std::optional<LoadJobId> only) const
{
    std::vector<OperationRow> rows;
    const auto selected =
        only ? (states_.contains(*only)
                    ? std::vector<VectorLoadJobState>{states_.at(*only)}
                    : std::vector<VectorLoadJobState>{})
             : jobStates();
    for (const VectorLoadJobState &state : selected) {
        std::string title = std::string("Vector import");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path source =
                job->second.request.sourcePath.filename();
            if (!source.empty()) {
                title = operationPath(source);
            }
        }
        const std::size_t selected = state.summary.selected.size();
        const std::size_t succeeded = state.summary.successful.size();
        const std::size_t failed = state.summary.failed.size();
        std::string detail = state.detail;
        if (state.phase == VectorLoadJobPhase::Ready) {
            detail = failed == 0
                         ? std::format("Loaded {0} of {1}", succeeded, selected)
                         : std::format("Loaded {0} of {1} · {2} failed "
                                       "— Retry failed sublayers",
                                       succeeded,
                                       selected,
                                       failed);
        } else if (state.phase == VectorLoadJobPhase::Failed) {
            detail = std::format("No sublayers loaded · {0} failed", failed);
        } else if (state.phase == VectorLoadJobPhase::Cancelled) {
            detail = std::format(
                "Cancelled after {0} of {1} · loaded layers retained",
                succeeded,
                selected);
        } else if (state.phase == VectorLoadJobPhase::Reading) {
            detail = std::format("Reading {0} of {1} · {2} failed",
                                 succeeded + failed,
                                 selected,
                                 failed);
        }
        const bool terminal = state.phase == VectorLoadJobPhase::Ready ||
                              state.phase == VectorLoadJobPhase::Failed ||
                              state.phase == VectorLoadJobPhase::Cancelled;
        const double completion =
            terminal ? 1.0
            : selected == 0
                ? 0.0
                : std::clamp(static_cast<double>(succeeded + failed) /
                                 static_cast<double>(selected),
                             0.0,
                             1.0);
        rows.push_back({
            .key = {.kind = LoadJobKind::Vector, .id = state.jobId},
            .title = title,
            .detail = std::format("Vector · {0}", detail),
            .completion = completion,
            .terminal = terminal,
            .capabilities = terminal
                                ? terminalLoadJobCapabilities(state.canRetry)
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

bool VectorImportOperation::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !found->second.lifecycle.finished())
        return false;
    jobs_.erase(found);
    states_.erase(jobId);
    return true;
}

void VectorImportOperation::setState(const LoadJobId jobId,
                                     const VectorLoadJobPhase phase,
                                     std::string detail)
{
    Job &job = jobs_.at(jobId);
    job.phase = phase;
    VectorLoadJobState &state = states_.at(jobId);
    state.phase = phase;
    state.detail = std::move(detail);
    state.canCancel = phase != VectorLoadJobPhase::Ready &&
                      phase != VectorLoadJobPhase::Failed &&
                      phase != VectorLoadJobPhase::Cancelled;
    state.canRetry = phase == VectorLoadJobPhase::Ready
                         ? !job.summary.failed.empty()
                         : ((phase == VectorLoadJobPhase::Failed ||
                             phase == VectorLoadJobPhase::Cancelled) &&
                            !job.summary.selected.empty());
    notifyJobStateChanged(jobId);
}

void VectorImportOperation::emitTerminal(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.lifecycle.finished())
        return;
    Job &job = found->second;
    static_cast<void>(job.lifecycle.finish());
    VectorLoadJobState &state = states_.at(jobId);
    state.summary = job.summary;
    if (job.cancelled) {
        setState(jobId, VectorLoadJobPhase::Cancelled);
        notifyCancelled(jobId, job.summary);
    } else if (!job.summary.successful.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Ready,
                 job.summary.partialSuccess()
                     ? std::string("Some sublayers failed")
                     : std::string{});
        notifyFinished(jobId, job.summary);
        notifyLoaded(jobId, job.summary);
    } else {
        const std::string message =
            state.detail.empty()
                ? std::string("No selected vector sublayers could be loaded")
                : state.detail;
        setState(jobId, VectorLoadJobPhase::Failed, message);
        notifyFailed(jobId, message);
    }
}

const TaskScheduler *VectorImportOperation::schedulerIdentity() const noexcept
{
    return scheduler_;
}

} // namespace pci
