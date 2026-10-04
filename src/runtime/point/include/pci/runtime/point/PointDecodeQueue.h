#pragma once

#include <algorithm>
#include <pci/runtime/PointMemoryBudget.h>
#include <pci/runtime/point/DecodedPageCache.h>

#include <cstddef>
#include <memory>
#include <mutex>

namespace pci {

// A reserved completion slot travels with its request, including while queued
// or executing. Completed requests retain the slot until owner-thread admission
// or cancellation. No producer ever waits for the consumer to make progress.
class PointDecodeQueue final {
    struct State {
        std::mutex mutex;
        std::size_t occupied = 0;
    };

public:
    static constexpr std::size_t capacity = 64;

    class Slot final {
    public:
        ~Slot()
        {
            if (counted_) {
                const std::scoped_lock lock(state_->mutex);
                --state_->occupied;
            }
        }
        Slot(const Slot &) = delete;
        Slot &operator=(const Slot &) = delete;

        [[nodiscard]] bool fit(std::uint64_t bytes)
        {
            return reservation_->tryResize(bytes);
        }

    private:
        friend class PointDecodeQueue;
        Slot(std::shared_ptr<State> state,
             PointMemoryBudget::ReservationPtr reservation)
            : state_(std::move(state))
            , reservation_(std::move(reservation))
        {
        }
        bool counted_ = false;
        std::shared_ptr<State> state_;
        PointMemoryBudget::ReservationPtr reservation_;
    };

    explicit PointDecodeQueue(PointMemoryBudgetPtr budget,
                              DecodedPageCachePtr cache = {})
        : budget_(std::move(budget))
        , cache_(std::move(cache))
    {
    }

    [[nodiscard]] std::shared_ptr<Slot> reserve(std::uint64_t bytes)
    {
        std::shared_ptr<Slot> result;
        {
            const std::scoped_lock lock(state_->mutex);
            if (state_->occupied == capacity) {
                return {};
            }
            auto reservation = budget_->tryReserve(bytes);
            if (!reservation) {
                return {};
            }
            result = std::shared_ptr<Slot>(
                new Slot(state_, std::move(*reservation)));
            ++state_->occupied;
            result->counted_ = true;
        }
        syncCacheBudget();
        return result;
    }

    void syncCacheBudget() const
    {
        if (cache_) {
            cache_->setByteBudget(
                std::max<std::uint64_t>(1, budget_->availableBytes()));
        }
    }

private:
    PointMemoryBudgetPtr budget_;
    DecodedPageCachePtr cache_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

using PointDecodeQueuePtr = std::shared_ptr<PointDecodeQueue>;

} // namespace pci
