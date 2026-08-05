#include "import/PointCloudLoadController.h"

#include "foundation/CheckedArithmetic.h"
#include "import/QueuedControllerCallback.h"
#include "platform/ProcessMemory.h"
#include "platform/QtPath.h"

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <QDebug>
#endif
#include <algorithm>
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <chrono>
#endif
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace pci {
namespace {

template <typename Outcome, typename Completion>
void postOutcome(
    const std::shared_ptr<QueuedControllerTarget<PointCloudLoadController>>
        &target,
    const LoadJobId jobId,
    Outcome outcome,
    Completion completion)
{
    postToObject(target,
                 [jobId,
                  outcome = std::move(outcome),
                  completion = std::move(completion)](
                     PointCloudLoadController *controller) mutable {
                     completion(controller, jobId, std::move(outcome));
                 });
}

JobError cancelledError()
{
    return {
        .code = JobErrorCode::Cancelled,
        .message = "Point-cloud import cancelled",
    };
}

QString jobErrorMessage(const JobError &error)
{
    return QString::fromStdString(error.message);
}

} // namespace

double PointCloudLoadJobState::weightedCompletion() const noexcept
{
    const double fraction = total > 0
                                ? std::clamp(static_cast<double>(processed) /
                                                 static_cast<double>(total),
                                             0.0,
                                             1.0)
                                : 0.0;
    switch (phase) {
    case PointCloudLoadJobPhase::Queued:
        return 0.0;
    case PointCloudLoadJobPhase::Inspecting:
        return 0.03;
    case PointCloudLoadJobPhase::WaitingForResources:
        return 0.08;
    case PointCloudLoadJobPhase::Reading:
        return 0.08 + 0.62 * fraction;
    case PointCloudLoadJobPhase::Indexing:
        return 0.15 + 0.65 * fraction;
    case PointCloudLoadJobPhase::PreviewReady:
        return 0.85;
    case PointCloudLoadJobPhase::Ready:
    case PointCloudLoadJobPhase::Failed:
    case PointCloudLoadJobPhase::Cancelled:
        return 1.0;
    }
    return 0.0;
}

PointCloudBatchProgress weightedBatchProgress(
    const std::vector<PointCloudLoadJobState> &states) noexcept
{
    PointCloudBatchProgress result;
    result.sourceCount = states.size();
    for (const PointCloudLoadJobState &state : states) {
        result.completion += state.weightedCompletion();
        switch (state.phase) {
        case PointCloudLoadJobPhase::Ready:
            ++result.completedSources;
            break;
        case PointCloudLoadJobPhase::Failed:
            ++result.completedSources;
            ++result.failedSources;
            break;
        case PointCloudLoadJobPhase::Cancelled:
            ++result.completedSources;
            ++result.cancelledSources;
            break;
        default:
            break;
        }
    }
    if (!states.empty()) {
        result.completion /= static_cast<double>(states.size());
    }
    return result;
}

namespace {

QString pointJobTitle(const PointCloudLoadJobState &state)
{
    return displayPathName(state.sourcePath);
}

QString pointJobPhase(const PointCloudLoadJobState &state)
{
    switch (state.phase) {
    case PointCloudLoadJobPhase::Queued:
        return QStringLiteral("Queued");
    case PointCloudLoadJobPhase::Inspecting:
        return QStringLiteral("Inspecting");
    case PointCloudLoadJobPhase::WaitingForResources:
        return QStringLiteral("Waiting for resources");
    case PointCloudLoadJobPhase::Reading:
        return QStringLiteral("Reading");
    case PointCloudLoadJobPhase::Indexing:
        return QStringLiteral("Indexing");
    case PointCloudLoadJobPhase::PreviewReady:
        return QStringLiteral("Preview ready");
    case PointCloudLoadJobPhase::Ready:
        return QStringLiteral("Loaded");
    case PointCloudLoadJobPhase::Failed:
        return QStringLiteral("Failed");
    case PointCloudLoadJobPhase::Cancelled:
        return QStringLiteral("Cancelled");
    }
    return {};
}

} // namespace

PointCloudLoadController::PointCloudLoadController(
    std::shared_ptr<const PointCloudLoader> loader,
    TaskScheduler &scheduler,
    QObject *parent)
    : QObject(parent)
    , loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("point-cloud loader must not be null");
    }
    callbackTarget_ =
        std::make_shared<QueuedControllerTarget<PointCloudLoadController>>(
            this);
    qRegisterMetaType<PointCloudScenePtr>();
    qRegisterMetaType<PointCloudImportStage>();
    qRegisterMetaType<LoadJobId>();
}

PointCloudLoadController::~PointCloudLoadController()
{
    callbackTarget_->invalidate();
    destroying_ = true;
    for (auto &[jobId, job] : jobs_) {
        static_cast<void>(jobId);
        job.stop.request_stop();
        if (job.taskId != TaskId{}) {
            static_cast<void>(scheduler_->cancel(job.taskId));
        }
    }
    jobs_.clear();
    jobStates_.clear();
    batches_.clear();
}

const TaskScheduler *
PointCloudLoadController::schedulerIdentity() const noexcept
{
    return scheduler_;
}

LoadJobId PointCloudLoadController::load(PointCloudLoadRequest request)
{
    auto ids = loadBatch({std::move(request)});
    return ids.front();
}

LoadJobId PointCloudLoadController::load(PointCloudLoadOptions options)
{
    return load(PointCloudLoadRequest{
        .options = std::move(options),
        .resources = {},
    });
}

std::vector<LoadJobId>
PointCloudLoadController::loadBatch(std::vector<PointCloudLoadRequest> requests)
{
    if (requests.empty()) {
        return {};
    }
    if (nextBatchId_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("point-cloud batch ids are exhausted");
    }
    std::uint64_t defaultBudgetBytes =
        std::numeric_limits<std::uint64_t>::max();
    bool needsDefaultBudget = false;
    for (const PointCloudLoadRequest &request : requests) {
        if (!request.resources.memoryBudget) {
            needsDefaultBudget = true;
            defaultBudgetBytes = std::min(defaultBudgetBytes,
                                          request.resources.decodedByteBudget);
        } else if (request.resources.residency &&
                   request.resources.memoryBudget->byteBudget() !=
                       request.resources.residency->byteBudget()) {
            throw std::invalid_argument(
                "import memory and hierarchy budgets must match");
        }
    }
    PointMemoryBudgetPtr defaultBudget;
    if (needsDefaultBudget) {
        defaultBudget = std::make_shared<PointMemoryBudget>(defaultBudgetBytes);
        for (PointCloudLoadRequest &request : requests) {
            if (!request.resources.memoryBudget) {
                request.resources.memoryBudget = defaultBudget;
            }
        }
    }
    const std::uint64_t batchId = ++nextBatchId_;
    batches_.emplace(batchId,
                     Batch{
                         .remainingPreflights = requests.size(),
                     });

    std::vector<LoadJobId> ids;
    ids.reserve(requests.size());
    for (PointCloudLoadRequest &request : requests) {
        ids.push_back(createJob(std::move(request), batchId));
    }
    for (const LoadJobId id : ids) {
        scheduleInspection(id);
    }
    return ids;
}

LoadJobId PointCloudLoadController::createJob(PointCloudLoadRequest request,
                                              const std::uint64_t batchId)
{
    const LoadJobId jobId = jobIds_.next("point-cloud job ids are exhausted");
    Job &job = jobs_[jobId];
    job.stop = std::stop_source{};
    job.batchId = batchId;
    jobStates_.insert_or_assign(jobId,
                                PointCloudLoadJobState{
                                    .jobId = jobId,
                                    .sourcePath = request.options.sourcePath,
                                    .phase = PointCloudLoadJobPhase::Queued,
                                    .detail = {},
                                });
    emit jobStateChanged(jobId);
    job.options = std::move(request.options);
    job.resources = std::move(request.resources);
    return jobId;
}

void PointCloudLoadController::scheduleInspection(const LoadJobId jobId)
{
    Job &job = jobs_.at(jobId);
    job.phase = JobPhase::Inspecting;
    setJobState(jobId, PointCloudLoadJobPhase::Inspecting);
    const PointCloudLoadOptions options = job.options;
    const std::uint64_t decodedByteBudget = job.resources.decodedByteBudget;
    const std::stop_token stopToken = job.stop.get_token();
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    job.taskId = scheduler_->submit(
        TaskPriority::Inspection,
        0,
        [loader,
         callbackTarget,
         jobId,
         options,
         decodedByteBudget,
         stopToken]() mutable {
            InspectionOutcome outcome = [&]() -> InspectionOutcome {
                try {
                    PointCloudImportPreflight preflight =
                        loader->inspect(options, decodedByteBudget, stopToken);
                    if (stopToken.stop_requested()) {
                        return std::unexpected(cancelledError());
                    }
                    return preflight;
                } catch (const PointCloudImportCancelled &) {
                    return std::unexpected(cancelledError());
                } catch (const PointCloudImportError &error) {
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
            postOutcome(callbackTarget,
                        jobId,
                        std::move(outcome),
                        [](PointCloudLoadController *controller,
                           const LoadJobId id,
                           InspectionOutcome result) {
                            controller->finishInspection(id, std::move(result));
                        });
        },
        [callbackTarget, jobId] {
            postToObject(callbackTarget,
                         [jobId](PointCloudLoadController *controller) {
                             controller->finishQueuedCancellation(
                                 jobId, JobPhase::Inspecting);
                         });
        });
}

void PointCloudLoadController::finishInspection(const LoadJobId jobId,
                                                InspectionOutcome outcome)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != JobPhase::Inspecting) {
        return;
    }
    found->second.taskId = TaskId{};
    if ((!outcome && outcome.error().code == JobErrorCode::Cancelled) ||
        found->second.stop.stop_requested()) {
        removePreflightJob(jobId, true, {});
        return;
    }
    if (!outcome) {
        removePreflightJob(jobId, false, jobErrorMessage(outcome.error()));
        return;
    }

    found->second.preflight = std::move(*outcome);
    found->second.phase = JobPhase::WaitingForBatch;
    if (auto state = jobStates_.find(jobId); state != jobStates_.end()) {
        state->second.localPaging = found->second.preflight->localPaging;
        emit jobStateChanged(jobId);
    }
    setJobState(jobId, PointCloudLoadJobPhase::WaitingForResources);
    ++preflightCompleted_;
    emit schedulingChanged();
    advanceBatch(found->second.batchId);
}

void PointCloudLoadController::removePreflightJob(const LoadJobId jobId,
                                                  const bool wasCancelled,
                                                  const QString &error)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    const std::uint64_t batchId = found->second.batchId;
    jobs_.erase(found);
    if (wasCancelled) {
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        emit cancelled(jobId);
    } else {
        ++preflightFailed_;
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, error);
        emit failed(jobId, error);
    }
    emit schedulingChanged();
    advanceBatch(batchId);
}

void PointCloudLoadController::advanceBatch(const std::uint64_t batchId)
{
    const auto found = batches_.find(batchId);
    if (found == batches_.end()) {
        return;
    }
    if (found->second.remainingPreflights == 0) {
        return;
    }
    --found->second.remainingPreflights;
    if (found->second.remainingPreflights == 0) {
        prepareBatch(batchId);
    }
}

void PointCloudLoadController::prepareBatch(const std::uint64_t batchId)
{
    std::vector<LoadJobId> jobIds;
    for (const auto &[jobId, job] : jobs_) {
        if (job.batchId == batchId && job.phase == JobPhase::WaitingForBatch) {
            jobIds.push_back(jobId);
        }
    }
    std::ranges::sort(jobIds);
    prepareMemoryAdmissions(jobIds);
    emit schedulingChanged();
    for (const LoadJobId jobId : jobIds) {
        if (jobs_.contains(jobId)) {
            scheduleLoad(jobId);
        }
    }
    batches_.erase(batchId);
}

void PointCloudLoadController::prepareMemoryAdmissions(
    const std::vector<LoadJobId> &jobIds)
{
    struct Group {
        PointMemoryBudgetPtr budget;
        HierarchyResidencyCoordinatorPtr coordinator;
        std::vector<LoadJobId> flatJobs;
        std::vector<LoadJobId> hierarchyJobs;
        std::uint64_t incomingHierarchyRootBytes = 0;
    };
    std::unordered_map<PointMemoryBudget *, Group> groups;

    for (const LoadJobId jobId : jobIds) {
        Job &job = jobs_.at(jobId);
        const PointCloudImportPreflight &preflight = *job.preflight;
        if (!job.resources.memoryBudget) {
            job.preflight->retainedPointLimit = preflight.desiredRetainedPoints;
            continue;
        }
        Group &group = groups[job.resources.memoryBudget.get()];
        group.budget = job.resources.memoryBudget;
        if (job.resources.residency) {
            group.coordinator = job.resources.residency;
        }
        if (preflight.hierarchical) {
            group.hierarchyJobs.push_back(jobId);
            group.incomingHierarchyRootBytes =
                saturatingAdd(group.incomingHierarchyRootBytes,
                              static_cast<std::uint64_t>(
                                  sizeof(GpuPoint) + sizeof(PointAttributes)));
        } else {
            group.flatJobs.push_back(jobId);
        }
    }

    for (auto &[key, group] : groups) {
        static_cast<void>(key);
        std::uint64_t fixedBytes = group.incomingHierarchyRootBytes;
        if (group.coordinator) {
            fixedBytes = saturatingAdd(fixedBytes,
                                       group.coordinator->retainedRootBytes());
        }
        const std::uint64_t currentlyAvailable = group.budget->availableBytes();
        const std::uint64_t minimumBytes = estimatedFlatResidentBytes(1);
        std::uint64_t requiredMinimum = fixedBytes;
        for (std::size_t index = 0; index < group.flatJobs.size(); ++index) {
            requiredMinimum = saturatingAdd(requiredMinimum, minimumBytes);
        }
        if (requiredMinimum > currentlyAvailable) {
            const QString message =
                QStringLiteral(
                    "The document CPU budget cannot hold the existing/incoming "
                    "hierarchy roots and one minimum spatial preview per flat "
                    "source (%1 bytes available; %2 bytes required for %3 "
                    "sources). Increase --cpu-cache-mb or split the document.")
                    .arg(currentlyAvailable)
                    .arg(requiredMinimum)
                    .arg(group.flatJobs.size() + group.hierarchyJobs.size());
            for (const LoadJobId jobId : group.flatJobs) {
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                emit failed(jobId, message);
                ++preflightFailed_;
            }
            for (const LoadJobId jobId : group.hierarchyJobs) {
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                emit failed(jobId, message);
                ++preflightFailed_;
            }
            continue;
        }
        if (group.flatJobs.empty()) {
            continue;
        }
        const std::uint64_t available = currentlyAvailable - fixedBytes;

        std::uint64_t maximumDesired = 1;
        for (const LoadJobId jobId : group.flatJobs) {
            maximumDesired =
                std::max(maximumDesired,
                         jobs_.at(jobId).preflight->desiredRetainedPoints);
        }
        std::uint64_t low = 1;
        std::uint64_t high = maximumDesired;
        while (low < high) {
            const std::uint64_t middle = low + (high - low + 1) / 2;
            std::uint64_t required = 0;
            for (const LoadJobId jobId : group.flatJobs) {
                const std::uint64_t desired =
                    jobs_.at(jobId).preflight->desiredRetainedPoints;
                required = saturatingAdd(
                    required,
                    estimatedFlatResidentBytes(std::min(desired, middle)));
            }
            if (required <= available) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }

        for (const LoadJobId jobId : group.flatJobs) {
            Job &job = jobs_.at(jobId);
            const std::uint64_t desired = job.preflight->desiredRetainedPoints;
            const std::uint64_t limit = std::min(desired, low);
            const std::uint64_t bytes = estimatedFlatResidentBytes(limit);
            auto reservation = group.budget->tryReserve(bytes);
            if (!reservation) {
                const QString message =
                    QStringLiteral(
                        "The document CPU budget changed while admitting '%1'; "
                        "%2 bytes could not be reserved.")
                        .arg(pathToQString(job.options.sourcePath))
                        .arg(bytes);
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                emit failed(jobId, message);
                continue;
            }
            job.resources.flatReservation = std::move(*reservation);
            job.preflight->retainedPointLimit = limit;
            job.preflight->spatialPreview = limit < desired;
            admittedFlatReservationBytes_ =
                saturatingAdd(admittedFlatReservationBytes_, bytes);
            if (job.preflight->spatialPreview) {
                ++safetySampledSources_;
                if (auto state = jobStates_.find(jobId);
                    state != jobStates_.end()) {
                    state->second.safetySampled = true;
                    emit jobStateChanged(jobId);
                }
            }
        }
    }
}

void PointCloudLoadController::scheduleLoad(const LoadJobId jobId)
{
    Job &job = jobs_.at(jobId);
    if (job.preflight->estimatedActiveBytes > scheduler_->activeByteBudget()) {
        const QString message =
            QStringLiteral(
                "The estimated active decode allocation for '%1' is %2 bytes, "
                "above the scheduler allowance of %3 bytes.")
                .arg(pathToQString(job.options.sourcePath))
                .arg(job.preflight->estimatedActiveBytes)
                .arg(scheduler_->activeByteBudget());
        jobs_.erase(jobId);
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
        emit failed(jobId, message);
        emit schedulingChanged();
        return;
    }
    job.phase = JobPhase::Loading;
    setJobState(jobId,
                job.preflight->localPaging ? PointCloudLoadJobPhase::Indexing
                                           : PointCloudLoadJobPhase::Reading,
                0,
                job.preflight->metadata.sourcePointCount);
    const PointCloudLoadOptions options = job.options;
    const PointCloudLoadResources resources = job.resources;
    const PointCloudImportPreflight preflight = *job.preflight;
    const std::stop_token stopToken = job.stop.get_token();
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    PointCloudLoadContext context{
        .stopToken = stopToken,
        .progress =
            [callbackTarget, jobId](const PointCloudImportProgress progress) {
                postToObject(
                    callbackTarget,
                    [jobId, progress](PointCloudLoadController *controller) {
                        if (!controller->jobs_.contains(jobId)) {
                            return;
                        }
                        const auto state = controller->jobStates_.find(jobId);
                        const bool localPaging =
                            state != controller->jobStates_.end() &&
                            state->second.localPaging;
                        controller->setJobState(
                            jobId,
                            localPaging ? PointCloudLoadJobPhase::Indexing
                                        : PointCloudLoadJobPhase::Reading,
                            progress.processed,
                            progress.total);
                        emit controller->progressChanged(jobId,
                                                         progress.stage,
                                                         progress.processed,
                                                         progress.total);
                    });
            },
        .sceneReady =
            [callbackTarget, jobId](const PointCloudScenePtr scene) {
                postToObject(
                    callbackTarget,
                    [jobId, scene](PointCloudLoadController *controller) {
                        if (!controller->jobs_.contains(jobId)) {
                            return;
                        }
                        if (auto state = controller->jobStates_.find(jobId);
                            state != controller->jobStates_.end()) {
                            state->second.previewAvailable = true;
                        }
                        controller->setJobState(
                            jobId, PointCloudLoadJobPhase::PreviewReady);
                        emit controller->sceneReady(jobId, scene);
                    });
            },
    };
    const std::uint64_t estimate = preflight.estimatedActiveBytes;
    const TaskPriority priority = preflight.hierarchical
                                      ? TaskPriority::VisibleCoverage
                                      : TaskPriority::Import;
    job.taskId = scheduler_->submit(
        priority,
        estimate,
        [loader,
         callbackTarget,
         jobId,
         options,
         resources,
         preflight,
         context = std::move(context)]() mutable {
            LoadOutcome outcome = [&]() -> LoadOutcome {
                try {
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                    const bool profile =
                        qEnvironmentVariableIsSet("PCI_PROFILE_LOADING");
                    const auto decodeStart = std::chrono::steady_clock::now();
#endif
                    PointCloudScenePtr scene =
                        loader->load(options, resources, preflight, context);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                    if (profile) {
                        const double seconds =
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - decodeStart)
                                .count();
                        qInfo().noquote()
                            << QStringLiteral(
                                   "[profile] decode %1: %2 pts in %3 s")
                                   .arg(displayPathName(options.sourcePath))
                                   .arg(scene ? scene->totalPointCount() : 0)
                                   .arg(seconds, 0, 'f', 3);
                    }
#endif
                    if (context.stopToken.stop_requested()) {
                        return std::unexpected(cancelledError());
                    }
                    if (!scene) {
                        return std::unexpected(JobError{
                            .code = JobErrorCode::Internal,
                            .message = "Point-cloud loader returned no scene",
                        });
                    }
                    return scene;
                } catch (const PointCloudImportCancelled &) {
                    return std::unexpected(cancelledError());
                } catch (const PointCloudImportError &error) {
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
            postOutcome(callbackTarget,
                        jobId,
                        std::move(outcome),
                        [](PointCloudLoadController *controller,
                           const LoadJobId id,
                           LoadOutcome result) {
                            controller->finishLoad(id, std::move(result));
                        });
        },
        [callbackTarget, jobId] {
            postToObject(callbackTarget,
                         [jobId](PointCloudLoadController *controller) {
                             controller->finishQueuedCancellation(
                                 jobId, JobPhase::Loading);
                         });
        });
}

void PointCloudLoadController::finishLoad(const LoadJobId jobId,
                                          LoadOutcome outcome)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != JobPhase::Loading) {
        return;
    }
    jobs_.erase(found);
    if (!outcome && outcome.error().code == JobErrorCode::Cancelled) {
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        emit cancelled(jobId);
    } else if (!outcome) {
        const QString message = jobErrorMessage(outcome.error());
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
        emit failed(jobId, message);
    } else {
        setJobState(jobId, PointCloudLoadJobPhase::Ready);
        emit loaded(jobId, *outcome);
    }
    emit schedulingChanged();
}

void PointCloudLoadController::finishQueuedCancellation(const LoadJobId jobId,
                                                        const JobPhase phase)
{
    if (destroying_) {
        return;
    }
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != phase) {
        return;
    }
    found->second.taskId = TaskId{};
    if (phase == JobPhase::Inspecting) {
        removePreflightJob(jobId, true, {});
    } else {
        jobs_.erase(found);
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        emit cancelled(jobId);
        emit schedulingChanged();
    }
}

void PointCloudLoadController::cancel()
{
    std::vector<LoadJobId> ids;
    ids.reserve(jobs_.size());
    for (const auto &[jobId, job] : jobs_) {
        static_cast<void>(job);
        ids.push_back(jobId);
    }
    for (const LoadJobId jobId : ids) {
        cancel(jobId);
    }
}

void PointCloudLoadController::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    found->second.stop.request_stop();
    if (found->second.phase == JobPhase::WaitingForBatch) {
        jobs_.erase(found);
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        emit cancelled(jobId);
        emit schedulingChanged();
        return;
    }
    if (found->second.taskId != TaskId{}) {
        static_cast<void>(scheduler_->cancel(found->second.taskId));
    }
}

bool PointCloudLoadController::prioritize(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.taskId == TaskId{}) {
        return false;
    }
    return scheduler_->reprioritize(found->second.taskId,
                                    TaskPriority::VisibleCoverage);
}

std::optional<PointCloudLoadJobState>
PointCloudLoadController::jobState(const LoadJobId jobId) const
{
    const auto found = jobStates_.find(jobId);
    return found == jobStates_.end()
               ? std::nullopt
               : std::optional<PointCloudLoadJobState>(found->second);
}

std::vector<PointCloudLoadJobState> PointCloudLoadController::jobStates() const
{
    return sortedJobStates<PointCloudLoadJobState>(jobStates_);
}

std::vector<LoadJobRow> PointCloudLoadController::jobRows() const
{
    std::vector<LoadJobRow> rows;
    for (const PointCloudLoadJobState &state : jobStates()) {
        const bool terminal = state.phase == PointCloudLoadJobPhase::Ready ||
                              state.phase == PointCloudLoadJobPhase::Failed ||
                              state.phase == PointCloudLoadJobPhase::Cancelled;
        rows.push_back({
            .key = {.kind = LoadJobKind::PointCloud, .id = state.jobId},
            .title = pointJobTitle(state),
            .detail = state.detail.isEmpty()
                          ? pointJobPhase(state)
                          : pointJobPhase(state) + QStringLiteral(" — ") +
                                state.detail,
            .completion = state.weightedCompletion(),
            .terminal = terminal,
            .capabilities = terminal
                                ? terminalLoadJobCapabilities(state.canRetry)
                                : activeLoadJobCapabilities(true),
        });
    }
    return rows;
}

bool PointCloudLoadController::dismiss(const LoadJobId jobId)
{
    if (jobs_.contains(jobId)) {
        return false;
    }
    return jobStates_.erase(jobId) > 0;
}

void PointCloudLoadController::setJobState(const LoadJobId jobId,
                                           const PointCloudLoadJobPhase phase,
                                           const std::uint64_t processed,
                                           const std::uint64_t total,
                                           QString detail)
{
    const auto found = jobStates_.find(jobId);
    if (found == jobStates_.end()) {
        return;
    }
    found->second.phase = phase;
    found->second.processed = processed;
    found->second.total = total;
    found->second.detail = std::move(detail);
    found->second.canCancel = phase != PointCloudLoadJobPhase::Ready &&
                              phase != PointCloudLoadJobPhase::Failed &&
                              phase != PointCloudLoadJobPhase::Cancelled;
    found->second.canRetry = phase == PointCloudLoadJobPhase::Failed ||
                             phase == PointCloudLoadJobPhase::Cancelled;
    emit jobStateChanged(jobId);
}

PointCloudLoadControllerMetrics PointCloudLoadController::metrics() const
{
    const ProcessMemoryMetrics memory = processMemoryMetrics();
    return {
        .scheduler = scheduler_->metrics(),
        .preflightCompleted = preflightCompleted_,
        .preflightFailed = preflightFailed_,
        .safetySampledSources = safetySampledSources_,
        .admittedFlatReservationBytes = admittedFlatReservationBytes_,
        .processResidentBytes = memory.residentBytes,
        .peakProcessResidentBytes = memory.peakResidentBytes,
    };
}

} // namespace pci
