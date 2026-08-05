#pragma once

#include "foundation/StrongId.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pci {

using TaskId = StrongId<struct TaskIdTag>;

enum class TaskPriority : std::uint8_t {
    Inspection,
    VisibleCoverage,
    VisibleDetail,
    Import,
    Background,
};

struct TaskSchedulerMetrics {
    std::uint64_t submitted = 0;
    std::uint64_t started = 0;
    std::uint64_t completed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t activeEstimatedBytes = 0;
    std::uint64_t peakActiveEstimatedBytes = 0;
    std::size_t active = 0;
    std::size_t peakActive = 0;
    std::size_t pending = 0;
    std::size_t peakPending = 0;
    std::size_t fairnessGroups = 0;
};

class TaskScheduler final {
public:
    using TaskId = pci::TaskId;
    using Task = std::function<void()>;

    static constexpr std::size_t defaultMaximumWorkers = 2;
    static constexpr std::uint64_t defaultActiveByteBudget =
        std::uint64_t{256} * 1024 * 1024;

    explicit TaskScheduler(
        std::size_t maximumWorkers = defaultMaximumWorkers,
        std::uint64_t activeByteBudget = defaultActiveByteBudget);
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler &) = delete;
    TaskScheduler &operator=(const TaskScheduler &) = delete;

    [[nodiscard]] TaskId submit(TaskPriority priority,
                                std::uint64_t estimatedBytes,
                                Task task,
                                Task cancelled = {},
                                std::uint64_t fairnessGroup = 0);
    // Returns true only when a queued task was removed. Active work must use
    // its request stop token for cooperative cancellation.
    [[nodiscard]] bool cancel(TaskId id);
    // Changes the priority of queued work without cancelling and rebuilding
    // its ownership/cancellation callbacks. Active work is intentionally not
    // preempted; callers should use cooperative cancellation for that case.
    [[nodiscard]] bool reprioritize(TaskId id, TaskPriority priority);
    void waitForIdle();

    [[nodiscard]] std::size_t maximumWorkers() const noexcept;
    [[nodiscard]] std::uint64_t activeByteBudget() const noexcept;
    [[nodiscard]] TaskSchedulerMetrics metrics() const;

private:
    struct Entry {
        TaskId id;
        TaskPriority priority = TaskPriority::Background;
        std::uint64_t sequence = 0;
        std::uint64_t estimatedBytes = 0;
        std::uint64_t fairnessGroup = 0;
        Task task;
        Task cancelled;
    };

    void workerLoop(std::stop_token stopToken);
    [[nodiscard]] std::deque<Entry>::iterator nextReadyTask();

    const std::size_t maximumWorkers_;
    const std::uint64_t activeByteBudget_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::condition_variable idle_;
    std::deque<Entry> pending_;
    std::unordered_map<std::uint64_t, std::uint64_t> groupLastStarted_;
    std::unordered_map<std::uint64_t, std::size_t> groupActive_;
    std::uint64_t fairnessClock_ = 0;
    std::vector<std::jthread> workers_;
    std::uint64_t nextIdValue_ = 1;
    std::uint64_t nextSequence_ = 1;
    std::size_t active_ = 0;
    std::size_t peakActive_ = 0;
    std::size_t peakPending_ = 0;
    std::uint64_t activeEstimatedBytes_ = 0;
    std::uint64_t peakActiveEstimatedBytes_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t started_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t cancelled_ = 0;
};

} // namespace pci
