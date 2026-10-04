#pragma once

#include <pci/operations/OperationRegistry.h>
#include <pci/runtime/CompletionExecutor.h>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace pci {

struct OperationCompletionSlot {
    CompletionExecutor::Completion completion;
    bool delivered = false;
};
template <typename Controller> class ReservedOperationCompletion;

// Workers retain this token rather than inspecting QObject state. The
// controller pointer is read and invalidated only on its owner thread, while
// the injected executor synchronizes worker acceptance with invalidation.
template <typename Controller>
class OperationTarget final
    : public std::enable_shared_from_this<OperationTarget<Controller>> {
public:
    OperationTarget(Controller *controller,
                    std::shared_ptr<CompletionExecutor> executor)
        : controller_(controller)
        , executor_(std::move(executor))
    {
        if (controller_ == nullptr || !executor_) {
            throw std::invalid_argument(
                "a queued controller target requires an owner and executor");
        }
    }

    OperationTarget(const OperationTarget &) = delete;
    OperationTarget &operator=(const OperationTarget &) = delete;

    void invalidate() noexcept
    {
        controller_ = nullptr;
        std::vector<std::shared_ptr<OperationCompletionSlot>> retired;
        {
            const std::scoped_lock lock(progressMutex_);
            accepting_ = false;
            progress_.clear();
            retired = std::move(terminals_);
            for (const auto &slot : retired)
                slot->delivered = true;
        }
        for (const auto &slot : retired)
            slot->completion = {};
        queue_.invalidate();
        executor_->invalidate();
    }

    [[nodiscard]] Controller *controller() const noexcept
    {
        return controller_;
    }

    [[nodiscard]] bool post(CompletionExecutor::Completion completion)
    {
        return queue_.post(std::move(completion), [this] {
            return requestWake();
        });
    }

    // A recovery tick must not consume an already accepted executor wake.
    void recover()
    {
        {
            const std::scoped_lock lock(wakeMutex_);
            if (!recoveryRequired_ || wakePending_)
                return;
            // Reserve this owner drain before releasing the wake protocol lock.
            wakePending_ = true;
        }
        drain();
    }

    // Only the owner calls drain, either from deferred delivery or recovery.
    void drain()
    {
        const auto keepAlive = this->shared_from_this();
        {
            const std::scoped_lock lock(wakeMutex_);
            recoveryRequired_ = false;
            wakePending_ = false;
        }
        queue_.drain();
        // Take one at a time: delivery may reserve another slot or destroy a
        // job.
        for (;;) {
            CompletionExecutor::Completion completion;
            {
                const std::scoped_lock lock(progressMutex_);
                const auto found =
                    std::ranges::find_if(terminals_, [](const auto &slot) {
                        return bool(slot->completion);
                    });
                if (found == terminals_.end())
                    break;
                completion = std::move((*found)->completion);
                (*found)->delivered = true;
                terminals_.erase(found);
            }
            if (completion)
                completion();
        }
    }

    // Reserve owner-delivery storage before admitting a worker. Terminal
    // results never compete with progress and survive failed executor wake
    // requests.
    std::shared_ptr<ReservedOperationCompletion<Controller>>
    reserveCompletion();
    bool postReserved(const std::shared_ptr<OperationCompletionSlot> &slot,
                      CompletionExecutor::Completion completion)
    {
        {
            const std::scoped_lock lock(progressMutex_);
            if (!accepting_ || slot->delivered || slot->completion)
                return false;
            slot->completion = std::move(completion);
        }
        static_cast<void>(requestWake());
        return true;
    }

    [[nodiscard]] bool postProgress(OperationToken token,
                                    CompletionExecutor::Completion completion,
                                    std::size_t part = 0)
    {
        const auto key =
            std::tuple{token.id.value(), token.attempt.value(), part};
        const std::scoped_lock lock(progressMutex_);
        if (!accepting_)
            return false;
        const bool pending = progress_.contains(key);
        progress_.insert_or_assign(key, std::move(completion));
        if (pending)
            return true;
        const auto weak = this->weak_from_this();
        return post(CompletionExecutor::Completion{[weak, key] {
            const auto target = weak.lock();
            if (!target)
                return;
            CompletionExecutor::Completion delivery;
            {
                const std::scoped_lock guard(target->progressMutex_);
                const auto found = target->progress_.find(key);
                if (found == target->progress_.end())
                    return;
                delivery = std::move(found->second);
                target->progress_.erase(found);
            }
            if (delivery)
                delivery();
        }});
    }

private:
    bool requestWake()
    {
        const std::scoped_lock lock(wakeMutex_);
        if (wakePending_)
            return true;
        wakePending_ = true;
        const auto weak = this->weak_from_this();
        try {
            if (executor_->post(CompletionExecutor::Completion{[weak] {
                    if (const auto target = weak.lock())
                        target->drain();
                }}))
                return true;
        } catch (...) {
            // Retained work is drained by the owner's recovery tick.
        }
        wakePending_ = false;
        recoveryRequired_ = true;
        return false;
    }
    std::mutex wakeMutex_;
    bool recoveryRequired_ = false;
    bool wakePending_ = false;
    std::vector<std::shared_ptr<OperationCompletionSlot>> terminals_;
    Controller *controller_;
    std::shared_ptr<CompletionExecutor> executor_;
    CompletionDispatchQueue queue_{true};
    std::mutex progressMutex_;
    bool accepting_ = true;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::size_t>,
             CompletionExecutor::Completion>
        progress_;
};

template <typename Controller> class ReservedOperationCompletion final {
public:
    ReservedOperationCompletion(
        std::shared_ptr<OperationTarget<Controller>> target,
        std::shared_ptr<OperationCompletionSlot> slot)
        : target_(std::move(target))
        , slot_(std::move(slot))
    {
    }
    std::shared_ptr<OperationTarget<Controller>> target_;
    std::shared_ptr<OperationCompletionSlot> slot_;
};
template <typename Controller>
std::shared_ptr<ReservedOperationCompletion<Controller>>
OperationTarget<Controller>::reserveCompletion()
{
    auto slot = std::make_shared<OperationCompletionSlot>();
    auto result = std::make_shared<ReservedOperationCompletion<Controller>>(
        this->shared_from_this(), slot);
    const std::scoped_lock lock(progressMutex_);
    if (!accepting_)
        throw std::logic_error("completion target is closed");
    // A submission that failed before publication leaves an unreferenced slot.
    std::erase_if(terminals_, [](const auto &entry) {
        return entry.use_count() == 1 && !entry->completion;
    });
    terminals_.push_back(std::move(slot));
    return result;
}
template <typename Controller, typename Completion>
bool postToOperation(
    const std::shared_ptr<ReservedOperationCompletion<Controller>> &reserved,
    Completion completion)
{
    std::weak_ptr<OperationTarget<Controller>> weak = reserved->target_;
    return reserved->target_->postReserved(
        reserved->slot_,
        CompletionExecutor::Completion{
            [weak, completion = std::move(completion)]() mutable {
                if (const auto target = weak.lock())
                    if (auto *owner = target->controller())
                        completion(owner);
            }});
}

template <typename Controller, typename Completion>
bool postToOperation(const std::shared_ptr<OperationTarget<Controller>> &target,
                     Completion completion)
{
    std::weak_ptr<OperationTarget<Controller>> weakTarget = target;
    return target->post(CompletionExecutor::Completion{
        [weakTarget, completion = std::move(completion)]() mutable {
            const auto liveTarget = weakTarget.lock();
            if (liveTarget) {
                if (Controller *controller = liveTarget->controller()) {
                    completion(controller);
                }
            }
        }});
}

template <typename Controller, typename Completion>
bool postOperationProgress(
    const std::shared_ptr<OperationTarget<Controller>> &target,
    OperationToken token,
    Completion completion,
    std::size_t part = 0)
{
    std::weak_ptr<OperationTarget<Controller>> weak = target;
    return target->postProgress(
        token,
        CompletionExecutor::Completion{
            [weak, completion = std::move(completion)]() mutable {
                if (const auto live = weak.lock())
                    if (auto *controller = live->controller())
                        completion(controller);
            }},
        part);
}

} // namespace pci
