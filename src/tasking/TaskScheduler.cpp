#include "tasking/TaskScheduler.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pci {

TaskScheduler::TaskScheduler(const std::size_t maximumWorkers,
                             const std::uint64_t activeByteBudget)
    : maximumWorkers_(maximumWorkers)
    , activeByteBudget_(activeByteBudget)
{
    if (maximumWorkers == 0) {
        throw std::invalid_argument(
            "task scheduler requires at least one worker");
    }
    if (activeByteBudget == 0) {
        throw std::invalid_argument(
            "task scheduler byte budget must be positive");
    }
    workers_.reserve(maximumWorkers_);
    for (std::size_t index = 0; index < maximumWorkers_; ++index) {
        workers_.emplace_back([this](const std::stop_token stopToken) {
            workerLoop(stopToken);
        });
    }
}

TaskScheduler::~TaskScheduler()
{
    std::vector<Task> cancellationCallbacks;
    {
        const std::scoped_lock lock(mutex_);
        cancellationCallbacks.reserve(pending_.size());
        for (Entry &entry : pending_) {
            if (entry.cancelled) {
                cancellationCallbacks.push_back(std::move(entry.cancelled));
            }
        }
        cancelled_ += pending_.size();
        pending_.clear();
    }
    for (Task &callback : cancellationCallbacks) {
        callback();
    }
    for (std::jthread &worker : workers_) {
        worker.request_stop();
    }
    ready_.notify_all();
}

TaskScheduler::TaskId TaskScheduler::submit(const TaskPriority priority,
                                            const std::uint64_t estimatedBytes,
                                            Task task,
                                            Task cancelled,
                                            const std::uint64_t fairnessGroup)
{
    if (!task) {
        throw std::invalid_argument("task scheduler task is empty");
    }
    if (estimatedBytes > activeByteBudget_) {
        throw std::invalid_argument(
            "task estimate exceeds the active byte budget");
    }

    TaskId id;
    {
        const std::scoped_lock lock(mutex_);
        if (nextIdValue_ == std::numeric_limits<std::uint64_t>::max() ||
            nextSequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "task scheduler identifiers are exhausted");
        }
        id = TaskId{nextIdValue_++};
        pending_.push_back({
            .id = id,
            .priority = priority,
            .sequence = nextSequence_++,
            .estimatedBytes = estimatedBytes,
            .fairnessGroup = fairnessGroup,
            .task = std::move(task),
            .cancelled = std::move(cancelled),
        });
        ++submitted_;
        peakPending_ = std::max(peakPending_, pending_.size());
    }
    ready_.notify_all();
    return id;
}

bool TaskScheduler::cancel(const TaskId id)
{
    Task callback;
    std::uint64_t fairnessGroup = 0;
    {
        const std::scoped_lock lock(mutex_);
        const auto found = std::ranges::find(pending_, id, &Entry::id);
        if (found == pending_.end()) {
            return false;
        }
        fairnessGroup = found->fairnessGroup;
        callback = std::move(found->cancelled);
        pending_.erase(found);
        const bool groupPending =
            std::ranges::any_of(pending_, [fairnessGroup](const Entry &entry) {
                return entry.fairnessGroup == fairnessGroup;
            });
        if (!groupPending && groupActive_[fairnessGroup] == 0) {
            groupActive_.erase(fairnessGroup);
            groupLastStarted_.erase(fairnessGroup);
        }
        ++cancelled_;
        if (pending_.empty() && active_ == 0) {
            idle_.notify_all();
        }
    }
    if (callback) {
        callback();
    }
    ready_.notify_all();
    return true;
}

bool TaskScheduler::reprioritize(const TaskId id, const TaskPriority priority)
{
    {
        const std::scoped_lock lock(mutex_);
        const auto found = std::ranges::find(pending_, id, &Entry::id);
        if (found == pending_.end()) {
            return false;
        }
        found->priority = priority;
    }
    ready_.notify_all();
    return true;
}

void TaskScheduler::waitForIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] {
        return pending_.empty() && active_ == 0;
    });
}

std::size_t TaskScheduler::maximumWorkers() const noexcept
{
    return maximumWorkers_;
}

std::uint64_t TaskScheduler::activeByteBudget() const noexcept
{
    return activeByteBudget_;
}

TaskSchedulerMetrics TaskScheduler::metrics() const
{
    const std::scoped_lock lock(mutex_);
    return {
        .submitted = submitted_,
        .started = started_,
        .completed = completed_,
        .cancelled = cancelled_,
        .activeEstimatedBytes = activeEstimatedBytes_,
        .peakActiveEstimatedBytes = peakActiveEstimatedBytes_,
        .active = active_,
        .peakActive = peakActive_,
        .pending = pending_.size(),
        .peakPending = peakPending_,
        .fairnessGroups = groupLastStarted_.size(),
    };
}

void TaskScheduler::workerLoop(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested()) {
        Entry entry;
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stopToken, [this] {
                    return nextReadyTask() != pending_.end();
                })) {
                return;
            }
            const auto found = nextReadyTask();
            if (found == pending_.end()) {
                continue;
            }
            entry = std::move(*found);
            pending_.erase(found);
            groupLastStarted_[entry.fairnessGroup] = ++fairnessClock_;
            ++groupActive_[entry.fairnessGroup];
            ++active_;
            ++started_;
            peakActive_ = std::max(peakActive_, active_);
            activeEstimatedBytes_ += entry.estimatedBytes;
            peakActiveEstimatedBytes_ =
                std::max(peakActiveEstimatedBytes_, activeEstimatedBytes_);
        }

        try {
            entry.task();
        } catch (...) {
            // Task boundaries own their error reporting. Never terminate a
            // scheduler worker because a client failed to translate an error.
        }

        {
            const std::scoped_lock lock(mutex_);
            --active_;
            activeEstimatedBytes_ -= entry.estimatedBytes;
            ++completed_;
            auto groupActive = groupActive_.find(entry.fairnessGroup);
            if (groupActive != groupActive_.end() && groupActive->second > 0) {
                --groupActive->second;
            }
            const bool groupPending =
                std::ranges::any_of(pending_, [&entry](const Entry &pending) {
                    return pending.fairnessGroup == entry.fairnessGroup;
                });
            if (!groupPending && (groupActive == groupActive_.end() ||
                                  groupActive->second == 0)) {
                groupActive_.erase(entry.fairnessGroup);
                groupLastStarted_.erase(entry.fairnessGroup);
            }
            if (pending_.empty() && active_ == 0) {
                idle_.notify_all();
            }
        }
        ready_.notify_all();
    }
}

std::deque<TaskScheduler::Entry>::iterator TaskScheduler::nextReadyTask()
{
    auto best = pending_.end();
    for (auto candidate = pending_.begin(); candidate != pending_.end();
         ++candidate) {
        if (candidate->estimatedBytes >
            activeByteBudget_ - activeEstimatedBytes_) {
            continue;
        }
        if (best == pending_.end() || candidate->priority < best->priority ||
            (candidate->priority == best->priority &&
             (groupLastStarted_[candidate->fairnessGroup] <
                  groupLastStarted_[best->fairnessGroup] ||
              (groupLastStarted_[candidate->fairnessGroup] ==
                   groupLastStarted_[best->fairnessGroup] &&
               candidate->sequence < best->sequence)))) {
            best = candidate;
        }
    }
    return best;
}

} // namespace pci
