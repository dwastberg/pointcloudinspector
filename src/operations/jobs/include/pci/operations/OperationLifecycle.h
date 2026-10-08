#pragma once

#include <pci/operations/OperationRegistry.h>
#include <pci/tasking/TaskScheduler.h>
#include <stop_token>
#include <utility>

namespace pci {

// Composed by each concrete job. Only its owner changes attempt/terminal state;
// workers receive the stop token and immutable attempt identity.
class OperationLifecycle final {
public:
    std::stop_source stop;
    AttemptGeneration generation{1};
    std::vector<TaskId> tasks;
    TaskId task;

    [[nodiscard]] bool finish() noexcept
    {
        return !std::exchange(terminal_, true);
    }
    [[nodiscard]] bool finished() const noexcept
    {
        return terminal_;
    }
    void restart()
    {
        const auto next = nextGeneration(generation);
        std::stop_source fresh;
        generation = next;
        stop = std::move(fresh);
        tasks.clear();
        task = {};
        terminal_ = false;
    }
    void cancel(TaskScheduler &scheduler)
    {
        stop.request_stop();
        if (task != TaskId{})
            static_cast<void>(scheduler.cancel(task));
        for (auto taskId : tasks)
            static_cast<void>(scheduler.cancel(taskId));
    }

private:
    bool terminal_ = false;
};

} // namespace pci
