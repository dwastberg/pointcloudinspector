#include <pci/operations/StatisticsOperation.h>

#include <algorithm>
#include <stdexcept>

namespace pci {
namespace {
JobResult<PointCloudStatistics> cancelledStatistics()
{
    return std::unexpected(
        JobError{JobErrorCode::Cancelled, "Statistics analysis cancelled."});
}
} // namespace

void StatisticsSubscription::reset() noexcept
{
    auto state = std::move(state_);
    if (!state)
        return;
    state->observer = {};
    auto cancel = std::move(state->cancel);
    if (cancel) {
        try {
            cancel();
        } catch (...) {
        }
    }
}

StatisticsOperation::StatisticsOperation(
    std::shared_ptr<const PointCloudStatisticsProvider> provider,
    TaskScheduler &scheduler,
    std::shared_ptr<CompletionExecutor> executor,
    std::shared_ptr<LoadJobIdSequence> ids,
    OperationRegistry &registry,
    Validator valid)
    : provider_(std::move(provider))
    , scheduler_(scheduler)
    , ids_(std::move(ids))
    , delivery_(std::make_shared<OperationTarget<StatisticsOperation>>(
          this, std::move(executor)))
    , valid_(std::move(valid))
{
    if (!provider_ || !ids_ || !valid_)
        throw std::invalid_argument(
            "statistics requires provider, identities, and binding validation");
    registration_.bind(registry, [this](LoadJobId id) {
        return OperationControls{[this, id] {
                                     cancel(id);
                                 },
                                 {},
                                 {},
                                 [this, id] {
                                     static_cast<void>(dismiss(id));
                                 }};
    });
}

StatisticsOperation::~StatisticsOperation()
{
    delivery_->invalidate();
    for (auto &[id, job] : jobs_) {
        static_cast<void>(id);
        job.observer.reset();
        job.lifecycle.cancel(scheduler_);
    }
    registration_.detach();
}

StatisticsSubscription
StatisticsOperation::start(PointCloudMetadata metadata,
                           StatisticsBindingToken binding,
                           StatisticsSubscription::Observer observer)
{
    StatisticsSubscription subscription;
    subscription.id_ = ids_->next("statistics operation ids are exhausted");
    subscription.state_ = std::make_shared<StatisticsSubscription::State>();
    subscription.state_->observer = std::move(observer);
    const auto id = subscription.id_;
    const auto token = OperationToken{id, AttemptGeneration{1}};
    const auto weak = std::weak_ptr(delivery_);
    subscription.state_->cancel = [weak, id] {
        if (const auto delivery = weak.lock())
            if (auto *owner = delivery->controller())
                owner->cancel(id);
    };
    Job job;
    job.lifecycle.tasks.reserve(1);
    job.binding = binding;
    job.observer = subscription.state_;
    job.update.total = metadata.sourcePointCount;
    job.row = {.key = {LoadJobKind::Statistics, id},
               .title =
                   "Statistics — " + operationPathName(metadata.sourcePath),
               .detail = "Queued for source statistics",
               .completion = 0,
               .terminal = false,
               .capabilities = activeLoadJobCapabilities(),
               .attempt = token.attempt,
               .state = OperationState::Queued,
               .target = binding.layer};
    const auto stop = job.lifecycle.stop.get_token();
    const auto delivery = delivery_;
    const auto provider = provider_;
    const auto completionTarget = delivery->reserveCompletion();
    jobs_.emplace(id, std::move(job));
    publish(id);
    const auto allowance = provider->estimatedWorkingBytes(metadata);
    auto complete = [completionTarget,
                     token](JobResult<PointCloudStatistics> result) {
        static_cast<void>(
            postToOperation(completionTarget,
                            [token, result = std::move(result)](
                                StatisticsOperation *owner) mutable {
                                owner->finish(token, std::move(result));
                            }));
    };
    // Validation/admission failures still deliver asynchronously, after the
    // caller has installed its subscription.
    if (!valid_(binding) || metadata.sourcePath.empty() ||
        metadata.sourceDriver.empty()) {
        complete(std::unexpected(JobError{
            JobErrorCode::Validation,
            "Full source statistics are unavailable for this point cloud."}));
    } else if (!allowance || allowance > scheduler_.activeByteBudget()) {
        complete(std::unexpected(JobError{
            JobErrorCode::ResourceAdmission,
            "Statistics working memory exceeds the operation budget."}));
    } else {
        try {
            auto task = scheduler_.submit(
                TaskPriority::Background,
                allowance,
                [provider,
                 metadata = std::move(metadata),
                 stop,
                 delivery,
                 token,
                 complete] {
                    if (stop.stop_requested()) {
                        complete(cancelledStatistics());
                        return;
                    }
                    static_cast<void>(postOperationProgress(
                        delivery, token, [token](StatisticsOperation *owner) {
                            owner->progress(token, 0, 0);
                        }));
                    try {
                        auto result = provider->calculate(
                            metadata,
                            stop,
                            [delivery, token](std::uint64_t processed,
                                              std::uint64_t total) {
                                static_cast<void>(postOperationProgress(
                                    delivery,
                                    token,
                                    [token, processed, total](
                                        StatisticsOperation *owner) {
                                        owner->progress(
                                            token, processed, total);
                                    }));
                            });
                        complete(stop.stop_requested()
                                     ? cancelledStatistics()
                                     : JobResult<PointCloudStatistics>(result));
                    } catch (const PointCloudStatisticsCancelled &) {
                        complete(cancelledStatistics());
                    } catch (const std::exception &error) {
                        complete(std::unexpected(
                            JobError{JobErrorCode::Internal, error.what()}));
                    } catch (...) {
                        complete(std::unexpected(
                            JobError{JobErrorCode::Internal,
                                     "Unknown statistics failure"}));
                    }
                },
                [complete] {
                    complete(cancelledStatistics());
                },
                id.value());
            jobs_.at(id).lifecycle.tasks.push_back(task);
        } catch (const std::exception &error) {
            complete(std::unexpected(
                JobError{JobErrorCode::ResourceAdmission, error.what()}));
        }
    }
    return subscription;
}

void StatisticsOperation::progress(OperationToken token,
                                   std::uint64_t processed,
                                   std::uint64_t total)
{
    const auto found = jobs_.find(token.id);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != token.attempt ||
        found->second.lifecycle.finished())
        return;
    auto &job = found->second;
    if (!valid_(job.binding)) {
        cancel(token.id);
        return;
    }
    job.update.processed = processed;
    if (total)
        job.update.total = total;
    job.row.state = OperationState::Running;
    job.row.detail = processed >= job.update.total && job.update.total
                         ? "Computing spatial outlier statistics…"
                         : "Scanning source statistics";
    job.row.completion =
        job.update.total ? std::clamp(static_cast<double>(processed) /
                                          static_cast<double>(job.update.total),
                                      0.0,
                                      0.99)
                         : 0;
    publish(token.id);
}

void StatisticsOperation::finish(OperationToken token,
                                 JobResult<PointCloudStatistics> result)
{
    const auto found = jobs_.find(token.id);
    if (found == jobs_.end() ||
        found->second.lifecycle.generation != token.attempt ||
        found->second.lifecycle.finished())
        return;
    auto &job = found->second;
    if (job.lifecycle.stop.stop_requested() || !valid_(job.binding))
        result = cancelledStatistics();
    auto prepared = std::make_shared<const JobResult<PointCloudStatistics>>(
        std::move(result));
    const auto &outcome = *prepared;
    job.row.state = outcome ? OperationState::Succeeded
                    : outcome.error().code == JobErrorCode::Cancelled
                        ? OperationState::Cancelled
                        : OperationState::Failed;
    job.row.detail = outcome ? "Statistics ready" : outcome.error().message;
    job.row.terminal = true;
    job.row.completion = 1;
    job.row.capabilities = terminalLoadJobCapabilities(false);
    job.update.result = std::move(prepared);
    static_cast<void>(job.lifecycle.finish());
    publish(token.id);
}

void StatisticsOperation::publish(LoadJobId id)
{
    const auto found = jobs_.find(id);
    if (found == jobs_.end())
        return;
    const auto observer = found->second.observer.lock();
    const auto update = found->second.update;
    const auto row = found->second.row;
    registration_.publish(row);
    if (observer && observer->observer) {
        try {
            const auto callback = observer->observer;
            callback(update);
        } catch (...) { /* Presentation cannot undo operation state. */
        }
    }
}
void StatisticsOperation::cancel(LoadJobId id)
{
    const auto found = jobs_.find(id);
    if (found != jobs_.end() && !found->second.lifecycle.finished())
        found->second.lifecycle.cancel(scheduler_);
}
void StatisticsOperation::cancelLayer(PointCloudLayerId id)
{
    for (auto &[jobId, job] : jobs_) {
        static_cast<void>(jobId);
        if (job.binding.layer == id && !job.lifecycle.finished())
            job.lifecycle.cancel(scheduler_);
    }
}
void StatisticsOperation::cancelInvalidTargets()
{
    for (auto &[id, job] : jobs_) {
        static_cast<void>(id);
        if (!job.lifecycle.finished() && !valid_(job.binding))
            job.lifecycle.cancel(scheduler_);
    }
}
void StatisticsOperation::cancelAll()
{
    for (auto &[id, job] : jobs_) {
        static_cast<void>(id);
        if (!job.lifecycle.finished())
            job.lifecycle.cancel(scheduler_);
    }
}
bool StatisticsOperation::dismiss(LoadJobId id)
{
    const auto found = jobs_.find(id);
    if (found == jobs_.end() || !found->second.lifecycle.finished())
        return false;
    jobs_.erase(found);
    registration_.remove(id);
    return true;
}
void StatisticsOperation::recoverDelivery()
{
    delivery_->recover();
}
} // namespace pci
