#include "import/VectorLoadController.h"

#include "foundation/CheckedArithmetic.h"
#include "import/QueuedControllerCallback.h"
#include "platform/QtPath.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace pci {
namespace {

constexpr std::uint64_t inspectionReservationBytes = std::uint64_t{1024} * 1024;

template <typename Completion>
void post(
    const std::shared_ptr<QueuedControllerTarget<VectorLoadController>> &target,
    Completion completion)
{
    postToObject(target, std::move(completion));
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

QString jobErrorMessage(const JobError &error)
{
    return QString::fromStdString(error.message);
}

} // namespace

VectorLoadController::VectorLoadController(
    std::shared_ptr<const VectorLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("vector loader must not be null");
    }
    callbackTarget_ =
        std::make_shared<QueuedControllerTarget<VectorLoadController>>(this);
    qRegisterMetaType<VectorLayerDataPtr>();
    qRegisterMetaType<VectorSublayerKey>();
    qRegisterMetaType<VectorLoadSummary>();
    qRegisterMetaType<VectorImportPreflight>();
    qRegisterMetaType<LoadJobId>();
}

VectorLoadController::~VectorLoadController()
{
    callbackTarget_->invalidate();
    cancelAll();
}

LoadJobId VectorLoadController::createJob(VectorImportRequest request,
                                          const VectorLoadJobPhase phase)
{
    const LoadJobId jobId = jobIds_.next("vector job ids are exhausted");
    Job job;
    job.request = std::move(request);
    job.request.stopToken = job.stop.get_token();
    job.phase = phase;
    jobs_.emplace(jobId, std::move(job));
    states_.emplace(jobId,
                    VectorLoadJobState{
                        .jobId = jobId,
                        .phase = phase,
                        .summary = {},
                        .detail = {},
                    });
    emit jobStateChanged(jobId);
    return jobId;
}

LoadJobId VectorLoadController::startInspection(VectorImportRequest request)
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

void VectorLoadController::scheduleInspection(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    const std::uint64_t generation = found->second.generation;
    const VectorImportRequest request = found->second.request;
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    const auto completeCancelled = [callbackTarget, jobId, generation] {
        post(
            callbackTarget,
            [jobId, generation](VectorLoadController *controller) {
                controller->finishInspection(
                    jobId, generation, std::unexpected(vectorCancelledError()));
            });
    };
    found->second.tasks.push_back(scheduler_->submit(
        TaskPriority::Inspection,
        inspectionReservationBytes,
        [callbackTarget, jobId, generation, loader, request] {
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
            post(callbackTarget,
                 [jobId, generation, result = std::move(result)](
                     VectorLoadController *controller) mutable {
                     controller->finishInspection(
                         jobId, generation, std::move(result));
                 });
        },
        completeCancelled,
        jobId.value()));
}

void VectorLoadController::finishInspection(const LoadJobId jobId,
                                            const std::uint64_t generation,
                                            InspectionResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation)
        return;
    Job &job = found->second;
    if (job.cancelled ||
        (!result && result.error().code == JobErrorCode::Cancelled)) {
        emitTerminal(jobId);
        return;
    }
    if (!result) {
        const QString message = jobErrorMessage(result.error());
        states_.at(jobId).detail = message;
        setState(jobId, VectorLoadJobPhase::Failed, message);
        emitTerminal(jobId);
        return;
    }
    if (result->sublayers.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Failed,
                 QStringLiteral("No vector sublayers were found"));
        emitTerminal(jobId);
        return;
    }
    job.preflight = std::move(*result);
    setState(jobId, VectorLoadJobPhase::AwaitingChoice);
    emit inspected(jobId, *job.preflight);
}

bool VectorLoadController::continueLoad(const LoadJobId jobId,
                                        std::vector<VectorSublayerKey> selected)
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

void VectorLoadController::beginLoad(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end())
        return;
    Job &job = found->second;
    if (job.request.sublayers.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Failed,
                 QStringLiteral("No vector sublayers were selected"));
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
        const std::uint64_t generation = job.generation;
        const VectorImportRequest request = job.request;
        const auto loader = loader_;
        const auto callbackTarget = callbackTarget_;
        const auto cancelled = [callbackTarget, jobId, generation] {
            post(callbackTarget,
                 [jobId, generation](VectorLoadController *controller) {
                     controller->finishOrigin(
                         jobId,
                         generation,
                         std::unexpected(vectorCancelledError()));
                 });
        };
        job.tasks.push_back(scheduler_->submit(
            TaskPriority::Inspection,
            inspectionReservationBytes,
            [callbackTarget, jobId, generation, request, loader] {
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
                post(callbackTarget,
                     [jobId, generation, result = std::move(result)](
                         VectorLoadController *controller) mutable {
                         controller->finishOrigin(
                             jobId, generation, std::move(result));
                     });
            },
            cancelled,
            jobId.value()));
        return;
    }
    setState(jobId, VectorLoadJobPhase::Reading);
    job.outstanding = job.request.sublayers.size();
    job.sublayerProgress.assign(job.request.sublayers.size(), {});
    VectorLoadJobState &state = states_.at(jobId);
    state.processed = 0;
    state.total = 0;
    const std::uint64_t generation = job.generation;
    for (const VectorSublayerKey &key : job.request.sublayers) {
        VectorImportRequest request = job.request;
        const auto loader = loader_;
        const auto callbackTarget = callbackTarget_;
        const auto cancelled = [callbackTarget, jobId, key, generation] {
            post(callbackTarget,
                 [jobId, key, generation](VectorLoadController *controller) {
                     controller->finishOne(
                         jobId,
                         key,
                         generation,
                         std::unexpected(vectorCancelledError()));
                 });
        };
        const auto downstreamProgress = request.progress;
        request.progress =
            [callbackTarget, jobId, key, generation, downstreamProgress](
                const VectorImportProgress progress) {
                if (downstreamProgress)
                    downstreamProgress(progress);
                post(callbackTarget,
                     [jobId, key, generation, progress](
                         VectorLoadController *controller) {
                         controller->reportProgress(
                             jobId, key, generation, progress);
                     });
            };
        const std::uint64_t estimatedBytes =
            request.limits.maximumApplicationWorkingBytes;
        job.tasks.push_back(scheduler_->submit(
            TaskPriority::Import,
            estimatedBytes,
            [callbackTarget,
             jobId,
             key,
             generation,
             request = std::move(request),
             loader] {
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
                post(callbackTarget,
                     [jobId, key, generation, result = std::move(result)](
                         VectorLoadController *controller) mutable {
                         controller->finishOne(
                             jobId, key, generation, std::move(result));
                     });
            },
            cancelled,
            jobId.value()));
    }
}

void VectorLoadController::reportProgress(const LoadJobId jobId,
                                          const VectorSublayerKey &key,
                                          const std::uint64_t generation,
                                          const VectorImportProgress progress)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation ||
        found->second.cancelled) {
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
    emit progressChanged(jobId, state.processed, state.total);
    emit jobStateChanged(jobId);
}

void VectorLoadController::finishOrigin(const LoadJobId jobId,
                                        const std::uint64_t generation,
                                        OriginResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation)
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

void VectorLoadController::finishOne(const LoadJobId jobId,
                                     VectorSublayerKey key,
                                     const std::uint64_t generation,
                                     SublayerResult result)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.generation != generation)
        return;
    Job &job = found->second;
    if (job.outstanding > 0)
        --job.outstanding;
    const bool wasCancelled =
        !result && result.error().code == JobErrorCode::Cancelled;
    if (!job.cancelled && !wasCancelled) {
        if (result) {
            if (!contains(job.summary.successful, key)) {
                job.summary.successful.push_back(key);
                emit sublayerLoaded(jobId, key, std::move(*result));
            }
        } else {
            const QString message = jobErrorMessage(result.error());
            job.summary.failed.push_back({key, result.error().message});
            emit sublayerFailed(jobId, key, message);
        }
        states_.at(jobId).summary = job.summary;
    }
    if (job.outstanding == 0)
        emitTerminal(jobId);
}

void VectorLoadController::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.terminalEmitted)
        return;
    Job &job = found->second;
    job.cancelled = true;
    job.stop.request_stop();
    for (const auto task : job.tasks)
        static_cast<void>(scheduler_->cancel(task));
    if (job.phase != VectorLoadJobPhase::Reading &&
        job.phase != VectorLoadJobPhase::ResolvingOrigin &&
        job.phase != VectorLoadJobPhase::Inspecting)
        emitTerminal(jobId);
}

void VectorLoadController::cancelAll()
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

bool VectorLoadController::retry(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !found->second.terminalEmitted)
        return false;
    Job &job = found->second;
    std::vector<VectorSublayerKey> retryKeys;
    for (const VectorSublayerKey &key : job.summary.selected) {
        if (!contains(job.summary.successful, key))
            retryKeys.push_back(key);
    }
    if (retryKeys.empty())
        return false;
    ++job.generation;
    job.stop = std::stop_source{};
    job.request.stopToken = job.stop.get_token();
    job.request.sublayers = std::move(retryKeys);
    job.summary.failed.clear();
    job.outstanding = 0;
    job.cancelled = false;
    job.terminalEmitted = false;
    job.tasks.clear();
    beginLoad(jobId);
    return true;
}

bool VectorLoadController::hasActiveJobs() const noexcept
{
    return std::ranges::any_of(jobs_, [](const auto &entry) {
        return !entry.second.terminalEmitted;
    });
}

std::optional<VectorLoadJobState>
VectorLoadController::jobState(const LoadJobId jobId) const
{
    const auto found = states_.find(jobId);
    return found == states_.end()
               ? std::nullopt
               : std::optional<VectorLoadJobState>(found->second);
}

std::vector<VectorLoadJobState> VectorLoadController::jobStates() const
{
    return sortedJobStates<VectorLoadJobState>(states_);
}

std::vector<LoadJobRow> VectorLoadController::jobRows() const
{
    std::vector<LoadJobRow> rows;
    for (const VectorLoadJobState &state : jobStates()) {
        QString title = QStringLiteral("Vector import");
        if (const auto job = jobs_.find(state.jobId); job != jobs_.end()) {
            const std::filesystem::path source =
                job->second.request.sourcePath.filename();
            if (!source.empty()) {
                title = pathToQString(source);
            }
        }
        const std::size_t selected = state.summary.selected.size();
        const std::size_t succeeded = state.summary.successful.size();
        const std::size_t failed = state.summary.failed.size();
        QString detail = state.detail;
        if (state.phase == VectorLoadJobPhase::Ready) {
            detail = failed == 0 ? QStringLiteral("Loaded %1 of %2")
                                       .arg(succeeded)
                                       .arg(selected)
                                 : QStringLiteral("Loaded %1 of %2 · %3 failed "
                                                  "— Retry failed sublayers")
                                       .arg(succeeded)
                                       .arg(selected)
                                       .arg(failed);
        } else if (state.phase == VectorLoadJobPhase::Failed) {
            detail =
                QStringLiteral("No sublayers loaded · %1 failed").arg(failed);
        } else if (state.phase == VectorLoadJobPhase::Cancelled) {
            detail = QStringLiteral(
                         "Cancelled after %1 of %2 · loaded layers retained")
                         .arg(succeeded)
                         .arg(selected);
        } else if (state.phase == VectorLoadJobPhase::Reading) {
            detail = QStringLiteral("Reading %1 of %2 · %3 failed")
                         .arg(succeeded + failed)
                         .arg(selected)
                         .arg(failed);
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
            .detail = QStringLiteral("Vector · %1").arg(detail),
            .completion = completion,
            .terminal = terminal,
            .capabilities = terminal
                                ? terminalLoadJobCapabilities(state.canRetry)
                                : activeLoadJobCapabilities(),
        });
    }
    return rows;
}

bool VectorLoadController::dismiss(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || !found->second.terminalEmitted)
        return false;
    jobs_.erase(found);
    states_.erase(jobId);
    return true;
}

void VectorLoadController::setState(const LoadJobId jobId,
                                    const VectorLoadJobPhase phase,
                                    QString detail)
{
    Job &job = jobs_.at(jobId);
    job.phase = phase;
    VectorLoadJobState &state = states_.at(jobId);
    state.phase = phase;
    state.detail = std::move(detail);
    state.canCancel = phase != VectorLoadJobPhase::Ready &&
                      phase != VectorLoadJobPhase::Failed &&
                      phase != VectorLoadJobPhase::Cancelled;
    state.canRetry = false;
    emit jobStateChanged(jobId);
}

void VectorLoadController::emitTerminal(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.terminalEmitted)
        return;
    Job &job = found->second;
    job.terminalEmitted = true;
    VectorLoadJobState &state = states_.at(jobId);
    state.summary = job.summary;
    if (job.cancelled) {
        setState(jobId, VectorLoadJobPhase::Cancelled);
        state.canRetry = !job.summary.selected.empty();
        emit cancelled(jobId, job.summary);
    } else if (!job.summary.successful.empty()) {
        setState(jobId,
                 VectorLoadJobPhase::Ready,
                 job.summary.partialSuccess()
                     ? QStringLiteral("Some sublayers failed")
                     : QString{});
        state.canRetry = !job.summary.failed.empty();
        emit finished(jobId, job.summary);
        emit loaded(jobId, job.summary);
    } else {
        const QString message =
            state.detail.isEmpty()
                ? QStringLiteral("No selected vector sublayers could be loaded")
                : state.detail;
        setState(jobId, VectorLoadJobPhase::Failed, message);
        state.canRetry = !job.summary.selected.empty();
        emit failed(jobId, message);
    }
}

const TaskScheduler *VectorLoadController::schedulerIdentity() const noexcept
{
    return scheduler_;
}

} // namespace pci
