#pragma once

#include <pci/runtime/CompletionExecutor.h>

#include <cstddef>
#include <mutex>
#include <utility>

namespace pci::test {

class DeterministicCompletionExecutor final : public CompletionExecutor {
public:
    [[nodiscard]] bool post(Completion completion) override
    {
        return queue_.post(std::move(completion), [this] {
            const std::scoped_lock lock(wakeMutex_);
            if (!acceptWakes_) {
                return false;
            }
            ++pendingWakes_;
            return true;
        });
    }

    void invalidate() noexcept override
    {
        queue_.invalidate();
    }

    [[nodiscard]] bool runNext()
    {
        {
            const std::scoped_lock lock(wakeMutex_);
            if (pendingWakes_ == 0) {
                return false;
            }
            --pendingWakes_;
        }
        queue_.drain();
        return true;
    }

    [[nodiscard]] std::size_t pendingWakeCount() const
    {
        const std::scoped_lock lock(wakeMutex_);
        return pendingWakes_;
    }

    void rejectWakeRequests()
    {
        const std::scoped_lock lock(wakeMutex_);
        acceptWakes_ = false;
    }

private:
    CompletionDispatchQueue queue_;
    mutable std::mutex wakeMutex_;
    std::size_t pendingWakes_ = 0;
    bool acceptWakes_ = true;
};

} // namespace pci::test
