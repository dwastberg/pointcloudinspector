#include <pci/runtime/CompletionExecutor.h>

#include <utility>

namespace pci {

bool CompletionDispatchQueue::post(Completion completion,
                                   const WakeRequest &requestWake)
{
    if (!completion || !requestWake) {
        return false;
    }

    std::deque<Completion> rejected;
    bool accepted = false;
    {
        const std::scoped_lock lock(mutex_);
        if (!accepting_) {
            return false;
        }

        completions_.push_back(std::move(completion));
        if (wakePending_) {
            return true;
        }

        wakePending_ = true;
        try {
            accepted = requestWake();
        } catch (...) {
            accepted = false;
        }
        if (!accepted) {
            wakePending_ = false;
            if (retainOnWakeFailure_)
                accepted = true;
            else
                rejected = std::exchange(completions_, {});
        }
    }
    return accepted;
}

void CompletionDispatchQueue::drain()
{
    std::deque<Completion> batch;
    {
        const std::scoped_lock lock(mutex_);
        wakePending_ = false;
        if (!accepting_) {
            completions_.clear();
            return;
        }
        batch = std::exchange(completions_, {});
    }

    for (Completion &completion : batch) {
        if (!accepting()) {
            return;
        }
        completion();
    }
}

void CompletionDispatchQueue::invalidate() noexcept
{
    std::deque<Completion> released;
    {
        const std::scoped_lock lock(mutex_);
        accepting_ = false;
        wakePending_ = false;
        released = std::exchange(completions_, {});
    }
}

bool CompletionDispatchQueue::accepting() const noexcept
{
    const std::scoped_lock lock(mutex_);
    return accepting_;
}

} // namespace pci
