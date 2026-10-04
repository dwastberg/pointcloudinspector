#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace pci {

struct PointMemoryBudgetMetrics {
    std::uint64_t byteBudget = 0;
    std::uint64_t reservedBytes = 0;
    std::uint64_t peakReservedBytes = 0;
    std::uint64_t reservationRequests = 0;
    std::uint64_t reservationFailures = 0;
};

class PointMemoryBudget final
    : public std::enable_shared_from_this<PointMemoryBudget> {
public:
    class Reservation final {
    public:
        ~Reservation();

        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;

        [[nodiscard]] std::uint64_t bytes() const noexcept;
        [[nodiscard]] std::shared_ptr<PointMemoryBudget> budget() const noexcept
        {
            return owner_;
        }
        // Resizing is atomic with respect to every other reservation. Growing
        // fails without changing the reservation when the document budget is
        // already exhausted.
        [[nodiscard]] bool tryResize(std::uint64_t bytes);
        // Creates a separately owned reservation against the same budget.
        // This is used for transactional staging when the owner of an existing
        // allocation must remain live until an atomic swap completes.
        [[nodiscard]] std::optional<std::shared_ptr<Reservation>>
        tryReserveSibling(std::uint64_t bytes) const;

    private:
        friend class PointMemoryBudget;
        Reservation(std::shared_ptr<PointMemoryBudget> owner,
                    std::uint64_t bytes) noexcept;

        std::shared_ptr<PointMemoryBudget> owner_;
        std::atomic_uint64_t bytes_ = 0;
    };

    using ReservationPtr = std::shared_ptr<Reservation>;

    explicit PointMemoryBudget(std::uint64_t byteBudget);

    [[nodiscard]] std::optional<ReservationPtr> tryReserve(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t byteBudget() const noexcept;
    // Changes the admission/cache ceiling without allocating memory. Lowering
    // below already reserved flat/root payloads is rejected because those
    // bytes are pinned until their owning scene releases them.
    [[nodiscard]] bool setByteBudget(std::uint64_t byteBudget);
    [[nodiscard]] std::uint64_t reservedBytes() const;
    [[nodiscard]] std::uint64_t availableBytes() const;
    [[nodiscard]] PointMemoryBudgetMetrics metrics() const;

private:
    friend class Reservation;
    [[nodiscard]] bool resize(Reservation &reservation, std::uint64_t bytes);
    void release(std::uint64_t bytes) noexcept;

    std::atomic_uint64_t byteBudget_;
    mutable std::mutex mutex_;
    std::uint64_t reservedBytes_ = 0;
    std::uint64_t peakReservedBytes_ = 0;
    std::uint64_t reservationRequests_ = 0;
    std::uint64_t reservationFailures_ = 0;
};

using PointMemoryBudgetPtr = std::shared_ptr<PointMemoryBudget>;

// Tie admission to the allocation, including leases that outlive its runtime.
// Declaration order releases the payload before releasing its reservation.
template <typename T>
[[nodiscard]] std::shared_ptr<T>
retainPointMemoryReservation(std::shared_ptr<T> payload,
                             PointMemoryBudget::ReservationPtr reservation)
{
    if (!payload || !reservation) {
        return payload;
    }
    struct Owner {
        PointMemoryBudget::ReservationPtr reservation;
        std::shared_ptr<T> payload;
    };
    auto owner = std::make_shared<Owner>(
        Owner{std::move(reservation), std::move(payload)});
    auto *pointer = owner->payload.get();
    return std::shared_ptr<T>(std::move(owner), pointer);
}

} // namespace pci
