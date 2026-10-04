#include <pci/operations/PointImportOperation.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/operations/OperationTarget.h>

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <cstdlib>
#include <iostream>
#endif
#include <algorithm>
#include <format>
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

template <typename Target, typename Outcome, typename Completion>
void postOutcome(const std::shared_ptr<Target> &target,
                 const LoadJobId jobId,
                 Outcome outcome,
                 Completion completion)
{
    postToOperation(target,
                    [jobId,
                     outcome = std::move(outcome),
                     completion = std::move(completion)](
                        PointImportOperation *controller) mutable {
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

std::string jobErrorMessage(const JobError &error)
{
    return std::string(error.message);
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

std::string pointJobTitle(const PointCloudLoadJobState &state)
{
    return operationPathName(state.sourcePath);
}

std::string pointJobPhase(const PointCloudLoadJobState &state)
{
    switch (state.phase) {
    case PointCloudLoadJobPhase::Queued:
        return std::string("Queued");
    case PointCloudLoadJobPhase::Inspecting:
        return std::string("Inspecting");
    case PointCloudLoadJobPhase::WaitingForResources:
        return std::string("Waiting for resources");
    case PointCloudLoadJobPhase::Reading:
        return std::string("Reading");
    case PointCloudLoadJobPhase::Indexing:
        return std::string("Indexing");
    case PointCloudLoadJobPhase::PreviewReady:
        return std::string("Preview ready");
    case PointCloudLoadJobPhase::Ready:
        return std::string("Loaded");
    case PointCloudLoadJobPhase::Failed:
        return std::string("Failed");
    case PointCloudLoadJobPhase::Cancelled:
        return std::string("Cancelled");
    }
    return {};
}

} // namespace

PointImportOperation::PointImportOperation(
    std::shared_ptr<const PointCloudLoader> loader,
    TaskScheduler &scheduler,
    std::shared_ptr<CompletionExecutor> executor)
    : loader_(std::move(loader))
    , scheduler_(&scheduler)
{
    if (!loader_) {
        throw std::invalid_argument("point-cloud loader must not be null");
    }
    callbackTarget_ = std::make_shared<OperationTarget<PointImportOperation>>(
        this, std::move(executor));
}

PointImportOperation::~PointImportOperation()
{
    events_ = {};
    registration_.detach();
    callbackTarget_->invalidate();
    destroying_ = true;
    for (auto &[jobId, job] : jobs_) {
        static_cast<void>(jobId);
        job.lifecycle.stop.request_stop();
        if (job.ingestion) {
            job.ingestion->close();
        }
        if (job.lifecycle.task != TaskId{}) {
            static_cast<void>(scheduler_->cancel(job.lifecycle.task));
        }
    }
    jobs_.clear();
    jobStates_.clear();
    batches_.clear();
}

const TaskScheduler *PointImportOperation::schedulerIdentity() const noexcept
{
    return scheduler_;
}

LoadJobId PointImportOperation::load(PointCloudLoadRequest request)
{
    auto ids = loadBatch({std::move(request)});
    return ids.front();
}

LoadJobId PointImportOperation::load(PointCloudLoadOptions options)
{
    return load(PointCloudLoadRequest{
        .options = std::move(options),
        .resources = {},
    });
}

std::vector<LoadJobId>
PointImportOperation::loadBatch(std::vector<PointCloudLoadRequest> requests)
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

LoadJobId PointImportOperation::createJob(PointCloudLoadRequest request,
                                          const std::uint64_t batchId)
{
    const LoadJobId jobId = jobIds_->next("point-cloud job ids are exhausted");
    Job &job = jobs_[jobId];
    job.lifecycle.stop = std::stop_source{};
    job.batchId = batchId;
    jobStates_.insert_or_assign(jobId,
                                PointCloudLoadJobState{
                                    .jobId = jobId,
                                    .sourcePath = request.options.sourcePath,
                                    .phase = PointCloudLoadJobPhase::Queued,
                                    .detail = {},
                                });
    notifyJobStateChanged(jobId);
    job.options = std::move(request.options);
    job.resources = std::move(request.resources);
    job.token = {request.session, jobId.value()};
    return jobId;
}

void PointImportOperation::scheduleInspection(const LoadJobId jobId)
{
    Job &job = jobs_.at(jobId);
    job.phase = JobPhase::Inspecting;
    setJobState(jobId, PointCloudLoadJobPhase::Inspecting);
    const PointCloudLoadOptions options = job.options;
    const std::uint64_t decodedByteBudget = job.resources.decodedByteBudget;
    const std::stop_token stopToken = job.lifecycle.stop.get_token();
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_->reserveCompletion();
    job.lifecycle.task = scheduler_->submit(
        TaskPriority::Inspection,
        0,
        [loader,
         options,
         callbackTarget,
         jobId,
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
                        [](PointImportOperation *controller,
                           const LoadJobId id,
                           InspectionOutcome result) {
                            controller->finishInspection(id, std::move(result));
                        });
        },
        [callbackTarget, jobId] {
            postToOperation(callbackTarget,
                            [jobId](PointImportOperation *controller) {
                                controller->finishQueuedCancellation(
                                    jobId, JobPhase::Inspecting);
                            });
        });
}

void PointImportOperation::finishInspection(const LoadJobId jobId,
                                            InspectionOutcome outcome)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != JobPhase::Inspecting) {
        return;
    }
    found->second.lifecycle.task = TaskId{};
    if ((!outcome && outcome.error().code == JobErrorCode::Cancelled) ||
        found->second.lifecycle.stop.stop_requested()) {
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
        notifyJobStateChanged(jobId);
    }
    setJobState(jobId, PointCloudLoadJobPhase::WaitingForResources);
    ++preflightCompleted_;
    notifySchedulingChanged();
    advanceBatch(found->second.batchId);
}

void PointImportOperation::removePreflightJob(const LoadJobId jobId,
                                              const bool wasCancelled,
                                              const std::string &error)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    const std::uint64_t batchId = found->second.batchId;
    static_cast<void>(found->second.lifecycle.finish());
    jobs_.erase(found);
    if (wasCancelled) {
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        notifyCancelled(jobId);
    } else {
        ++preflightFailed_;
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, error);
        notifyFailed(jobId, error);
    }
    notifySchedulingChanged();
    advanceBatch(batchId);
}

void PointImportOperation::advanceBatch(const std::uint64_t batchId)
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

void PointImportOperation::prepareBatch(const std::uint64_t batchId)
{
    std::vector<LoadJobId> jobIds;
    for (const auto &[jobId, job] : jobs_) {
        if (job.batchId == batchId && job.phase == JobPhase::WaitingForBatch) {
            jobIds.push_back(jobId);
        }
    }
    std::ranges::sort(jobIds);
    prepareMemoryAdmissions(jobIds);
    notifySchedulingChanged();
    for (const LoadJobId jobId : jobIds) {
        if (jobs_.contains(jobId)) {
            scheduleLoad(jobId);
        }
    }
    batches_.erase(batchId);
}

void PointImportOperation::prepareMemoryAdmissions(
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
            const std::string message = std::format(
                "The document CPU budget cannot hold the existing/incoming "
                "hierarchy roots and one minimum spatial preview per flat "
                "source ({0} bytes available; {1} bytes required for {2} "
                "sources). Increase --cpu-cache-mb or split the document.",
                currentlyAvailable,
                requiredMinimum,
                group.flatJobs.size() + group.hierarchyJobs.size());
            for (const LoadJobId jobId : group.flatJobs) {
                static_cast<void>(jobs_.at(jobId).lifecycle.finish());
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                notifyFailed(jobId, message);
                ++preflightFailed_;
            }
            for (const LoadJobId jobId : group.hierarchyJobs) {
                static_cast<void>(jobs_.at(jobId).lifecycle.finish());
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                notifyFailed(jobId, message);
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
                const std::string message = std::format(
                    "The document CPU budget changed while admitting '{0}'; "
                    "{1} bytes could not be reserved.",
                    operationPath(job.options.sourcePath),
                    bytes);
                static_cast<void>(jobs_.at(jobId).lifecycle.finish());
                jobs_.erase(jobId);
                setJobState(
                    jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
                notifyFailed(jobId, message);
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
                    notifyJobStateChanged(jobId);
                }
            }
        }
    }
}

void PointImportOperation::scheduleLoad(const LoadJobId jobId)
{
    Job &job = jobs_.at(jobId);
    if (job.preflight->estimatedActiveBytes > scheduler_->activeByteBudget()) {
        const std::string message = std::format(
            "The estimated active decode allocation for '{0}' is {1} bytes, "
            "above the scheduler allowance of {2} bytes.",
            operationPath(job.options.sourcePath),
            job.preflight->estimatedActiveBytes,
            scheduler_->activeByteBudget());
        static_cast<void>(jobs_.at(jobId).lifecycle.finish());
        jobs_.erase(jobId);
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
        notifyFailed(jobId, message);
        notifySchedulingChanged();
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
    const std::stop_token stopToken = job.lifecycle.stop.get_token();
    const auto loader = loader_;
    const auto callbackTarget = callbackTarget_;
    const auto pending = ingestionWakePending_;
    const auto failed = ingestionWakeFailed_;

    auto wake = [callbackTarget, pending, failed]() noexcept {
        if (!pending->exchange(true)) {
            try {
                if (!postToOperation(
                        callbackTarget,
                        [pending](PointImportOperation *controller) {
                            pending->store(false);
                            controller->drainIngestion();
                        })) {
                    pending->store(false);
                    failed->store(true);
                }
            } catch (...) {
                pending->store(false);
                failed->store(true);
            }
        }
    };
    const auto allowance = resources.flatReservation
                               ? resources.flatReservation->bytes()
                               : resources.decodedByteBudget;
    const auto ingestion =
        std::make_shared<PointDatasetIngestion>(allowance, wake);
    job.ingestion = ingestion;
    PointCloudLoadContext context{
        .stopToken = stopToken,
        .progress =
            [ingestion](PointCloudImportProgress progress) {
                ingestion->progress(progress);
            },
        .dataReady =
            [ingestion, stopToken](PointDatasetEvent event) {
                if (!ingestion->push(std::move(event), stopToken)) {
                    throw PointCloudImportCancelled();
                }
            },
        .token = job.token,
    };
    const std::uint64_t estimate = preflight.estimatedActiveBytes;
    const TaskPriority priority = preflight.hierarchical
                                      ? TaskPriority::VisibleCoverage
                                      : TaskPriority::Import;
    job.lifecycle.task = scheduler_->submit(
        priority,
        estimate,
        [loader,
         options,
         resources,
         preflight,
         ingestion,
         context = std::move(context)]() mutable {
            LoadOutcome outcome = [&]() -> LoadOutcome {
                try {
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                    const bool profile =
                        std::getenv("PCI_PROFILE_LOADING") != nullptr;
                    const auto decodeStart = std::chrono::steady_clock::now();
#endif
                    PreparedPointDatasetPtr scene =
                        loader->load(options, resources, preflight, context);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                    if (profile) {
                        const double seconds =
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - decodeStart)
                                .count();
                        std::clog
                            << std::format(
                                   "[profile] decode {0}: {1} pts in {2:.3f} s",
                                   operationPathName(options.sourcePath),
                                   scene ? scene->pointCount() : 0,
                                   seconds)
                            << '\n';
                    }
#endif
                    if (context.stopToken.stop_requested()) {
                        return std::unexpected(cancelledError());
                    }
                    if (!scene) {
                        return std::unexpected(JobError{
                            .code = JobErrorCode::Internal,
                            .message = "Point-cloud loader returned no "
                                       "prepared dataset",
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
            ingestion->finish(std::move(outcome));
        },
        [ingestion] {
            ingestion->finish(std::unexpected(cancelledError()));
        });
}

void PointImportOperation::requestIngestionWake()
{
    if (!ingestionWakePending_->exchange(true)) {
        const auto pending = ingestionWakePending_;
        try {
            if (!postToOperation(callbackTarget_,
                                 [pending](PointImportOperation *controller) {
                                     pending->store(false);
                                     controller->drainIngestion();
                                 })) {
                pending->store(false);
                ingestionWakeFailed_->store(true);
            }
        } catch (...) {
            pending->store(false);
            ingestionWakeFailed_->store(true);
        }
    }
}

void PointImportOperation::drainIngestion()
{
    std::vector<LoadJobId> ids;
    for (const auto &[id, job] : jobs_) {
        if (job.ingestion) {
            ids.push_back(id);
        }
    }
    std::ranges::sort(ids);
    const auto next = std::ranges::upper_bound(ids, lastDrainedJob_);
    std::rotate(ids.begin(), next, ids.end());
    std::size_t delivered = 0;
    bool more = false;
    bool roundDelivered;
    do {
        roundDelivered = false;
        for (const auto id : ids) {
            auto found = jobs_.find(id);
            if (found == jobs_.end()) {
                continue;
            }
            const auto queue = found->second.ingestion;
            if (found->second.lifecycle.stop.stop_requested()) {
                queue->close();
                finishLoad(id, std::unexpected(cancelledError()));
                continue;
            }
            if (auto progress = queue->takeProgress()) {
                const auto state = jobStates_.find(id);
                const bool local =
                    state != jobStates_.end() && state->second.localPaging;
                setJobState(id,
                            local ? PointCloudLoadJobPhase::Indexing
                                  : PointCloudLoadJobPhase::Reading,
                            progress->processed,
                            progress->total);
                notifyProgressChanged(
                    id, progress->stage, progress->processed, progress->total);
            }
            // One event per attempt per round prevents a fast reader starving
            // another.
            if (delivered < 8) {
                if (auto event = queue->takeEvent()) {
                    try {
                        found = jobs_.find(id);
                        if (found != jobs_.end() &&
                            !found->second.lifecycle.stop.stop_requested()) {
                            if (event->token != found->second.token ||
                                event->sequence != found->second.nextSequence) {
                                throw PointCloudImportError(
                                    "invalid point ingestion identity or "
                                    "sequence");
                            }
                            ++found->second.nextSequence;
                            notifyDataReady(id, *event);
                            if (event->sequence == 0 && jobs_.contains(id) &&
                                !jobs_.at(id).lifecycle.stop.stop_requested()) {
                                jobStates_.at(id).previewAvailable = true;
                                setJobState(
                                    id, PointCloudLoadJobPhase::PreviewReady);
                            }
                        }
                    } catch (const std::exception &error) {
                        rejectInstallation(id, std::string(error.what()));
                    }
                    event.reset();
                    queue->releaseEvent();
                    ++delivered;
                    roundDelivered = true;
                    lastDrainedJob_ = id;
                }
            }
            if (jobs_.contains(id)) {
                if (auto outcome = queue->takeOutcome()) {
                    finishLoad(id, std::move(*outcome));
                }
            }
            more = more || queue->pending();
        }

    } while (roundDelivered && delivered < 8);
    if (more) {
        requestIngestionWake();
    }
}

void PointImportOperation::rejectInstallation(const LoadJobId jobId,
                                              const std::string &message)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    found->second.lifecycle.stop.request_stop();
    if (found->second.ingestion) {
        found->second.ingestion->close();
    }
    static_cast<void>(found->second.lifecycle.finish());
    jobs_.erase(found);
    setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
    notifyFailed(jobId, message);
    notifySchedulingChanged();
}

void PointImportOperation::finishLoad(const LoadJobId jobId,
                                      LoadOutcome outcome)
{
    auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != JobPhase::Loading) {
        return;
    }
    if (found->second.lifecycle.stop.stop_requested()) {
        outcome = std::unexpected(cancelledError());
    }
    if (outcome) {
        try {
            if (!*outcome || !(*outcome)->loadingComplete ||
                (*outcome)->eventCount != found->second.nextSequence) {
                throw PointCloudImportError(
                    "point completion has an incomplete event stream");
            }
            // Direct owner-thread consumers prepare/commit before success is
            // reported. They may rejectInstallation(), removing this attempt.
            notifyPrepared(jobId, *outcome);
        } catch (const std::exception &error) {
            rejectInstallation(jobId, std::string(error.what()));
            return;
        }
        found = jobs_.find(jobId);
        if (found == jobs_.end()) {
            return;
        }
        if (found->second.lifecycle.stop.stop_requested()) {
            outcome = std::unexpected(cancelledError());
        }
    }
    if (found->second.ingestion) {
        found->second.ingestion->close();
    }
    static_cast<void>(found->second.lifecycle.finish());
    jobs_.erase(found);
    if (!outcome && outcome.error().code == JobErrorCode::Cancelled) {
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        notifyCancelled(jobId);
    } else if (!outcome) {
        const std::string message = jobErrorMessage(outcome.error());
        setJobState(jobId, PointCloudLoadJobPhase::Failed, 0, 0, message);
        notifyFailed(jobId, message);
    } else {
        setJobState(jobId, PointCloudLoadJobPhase::Ready);
        notifyLoaded(jobId, *outcome);
    }
    notifySchedulingChanged();
}

void PointImportOperation::finishQueuedCancellation(const LoadJobId jobId,
                                                    const JobPhase phase)
{
    if (destroying_) {
        return;
    }
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.phase != phase) {
        return;
    }
    found->second.lifecycle.task = TaskId{};
    if (phase == JobPhase::Inspecting) {
        removePreflightJob(jobId, true, {});
    } else {
        static_cast<void>(found->second.lifecycle.finish());
        jobs_.erase(found);
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        notifyCancelled(jobId);
        notifySchedulingChanged();
    }
}

void PointImportOperation::cancel()
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

void PointImportOperation::cancel(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    found->second.lifecycle.stop.request_stop();
    if (found->second.ingestion) {
        found->second.ingestion->close();
        requestIngestionWake();
    }
    if (found->second.phase == JobPhase::WaitingForBatch) {
        static_cast<void>(found->second.lifecycle.finish());
        jobs_.erase(found);
        setJobState(jobId, PointCloudLoadJobPhase::Cancelled);
        notifyCancelled(jobId);
        notifySchedulingChanged();
        return;
    }
    if (found->second.lifecycle.task != TaskId{}) {
        static_cast<void>(scheduler_->cancel(found->second.lifecycle.task));
    }
}

bool PointImportOperation::prioritize(const LoadJobId jobId)
{
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end() || found->second.lifecycle.task == TaskId{}) {
        return false;
    }
    return scheduler_->reprioritize(found->second.lifecycle.task,
                                    TaskPriority::VisibleCoverage);
}

std::optional<PointCloudLoadJobState>
PointImportOperation::jobState(const LoadJobId jobId) const
{
    const auto found = jobStates_.find(jobId);
    return found == jobStates_.end()
               ? std::nullopt
               : std::optional<PointCloudLoadJobState>(found->second);
}

std::vector<PointCloudLoadJobState> PointImportOperation::jobStates() const
{
    return sortedJobStates<PointCloudLoadJobState>(jobStates_);
}

std::vector<OperationRow>
PointImportOperation::jobRows(const std::optional<LoadJobId> only) const
{
    std::vector<OperationRow> rows;
    const auto selected =
        only ? (jobStates_.contains(*only)
                    ? std::vector<PointCloudLoadJobState>{jobStates_.at(*only)}
                    : std::vector<PointCloudLoadJobState>{})
             : jobStates();
    for (const PointCloudLoadJobState &state : selected) {
        const bool terminal = state.phase == PointCloudLoadJobPhase::Ready ||
                              state.phase == PointCloudLoadJobPhase::Failed ||
                              state.phase == PointCloudLoadJobPhase::Cancelled;
        rows.push_back({
            .key = {.kind = LoadJobKind::PointCloud, .id = state.jobId},
            .title = pointJobTitle(state),
            .detail =
                state.detail.empty()
                    ? pointJobPhase(state)
                    : pointJobPhase(state) + std::string(" — ") + state.detail,
            .completion = state.weightedCompletion(),
            .terminal = terminal,
            .capabilities = terminal
                                ? terminalLoadJobCapabilities(state.canRetry)
                                : activeLoadJobCapabilities(true),
            .attempt = jobs_.contains(state.jobId)
                           ? jobs_.at(state.jobId).lifecycle.generation
                           : AttemptGeneration{1},
            .state = operationState(state.phase),
            .target = std::nullopt,
        });
    }
    return rows;
}

bool PointImportOperation::dismiss(const LoadJobId jobId)
{
    if (jobs_.contains(jobId)) {
        return false;
    }
    const bool removed = jobStates_.erase(jobId) > 0;
    if (removed)
        notifyJobStateChanged(jobId);
    return removed;
}

void PointImportOperation::setJobState(const LoadJobId jobId,
                                       const PointCloudLoadJobPhase phase,
                                       const std::uint64_t processed,
                                       const std::uint64_t total,
                                       std::string detail)
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
    notifyJobStateChanged(jobId);
}

PointImportOperationMetrics PointImportOperation::operationMetrics() const
{
    return {
        .scheduler = scheduler_->metrics(),
        .preflightCompleted = preflightCompleted_,
        .preflightFailed = preflightFailed_,
        .safetySampledSources = safetySampledSources_,
        .admittedFlatReservationBytes = admittedFlatReservationBytes_,
    };
}

} // namespace pci
