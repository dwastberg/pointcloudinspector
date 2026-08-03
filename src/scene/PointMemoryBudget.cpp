#include "scene/PointMemoryBudget.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pci {

PointMemoryBudget::Reservation::Reservation(
    std::shared_ptr<PointMemoryBudget> owner,
    const std::uint64_t bytes) noexcept
    : owner_(std::move(owner))
    , bytes_(bytes)
{
}

PointMemoryBudget::Reservation::~Reservation()
{
    if (owner_) {
        owner_->release(bytes_.load(std::memory_order_relaxed));
    }
}

std::uint64_t PointMemoryBudget::Reservation::bytes() const noexcept
{
    return bytes_.load(std::memory_order_relaxed);
}

bool PointMemoryBudget::Reservation::tryResize(const std::uint64_t bytes)
{
    return owner_ && owner_->resize(*this, bytes);
}

PointMemoryBudget::PointMemoryBudget(const std::uint64_t byteBudget)
    : byteBudget_(byteBudget)
{
    if (byteBudget == 0) {
        throw std::invalid_argument(
            "point-memory budget must be greater than zero");
    }
}

std::optional<PointMemoryBudget::ReservationPtr>
PointMemoryBudget::tryReserve(const std::uint64_t bytes)
{
    {
        const std::scoped_lock lock(mutex_);
        ++reservationRequests_;
        const std::uint64_t budget =
            byteBudget_.load(std::memory_order_relaxed);
        if (bytes > budget - reservedBytes_) {
            ++reservationFailures_;
            return std::nullopt;
        }
        reservedBytes_ += bytes;
        peakReservedBytes_ = std::max(peakReservedBytes_, reservedBytes_);
    }

    try {
        return std::shared_ptr<Reservation>(
            new Reservation(shared_from_this(), bytes));
    } catch (...) {
        release(bytes);
        throw;
    }
}

std::uint64_t PointMemoryBudget::byteBudget() const noexcept
{
    return byteBudget_.load(std::memory_order_relaxed);
}

bool PointMemoryBudget::setByteBudget(const std::uint64_t byteBudget)
{
    if (byteBudget == 0) {
        return false;
    }
    const std::scoped_lock lock(mutex_);
    if (byteBudget < reservedBytes_) {
        return false;
    }
    byteBudget_.store(byteBudget, std::memory_order_relaxed);
    return true;
}

std::uint64_t PointMemoryBudget::reservedBytes() const
{
    const std::scoped_lock lock(mutex_);
    return reservedBytes_;
}

std::uint64_t PointMemoryBudget::availableBytes() const
{
    const std::scoped_lock lock(mutex_);
    return byteBudget_.load(std::memory_order_relaxed) - reservedBytes_;
}

PointMemoryBudgetMetrics PointMemoryBudget::metrics() const
{
    const std::scoped_lock lock(mutex_);
    return {
        .byteBudget = byteBudget_.load(std::memory_order_relaxed),
        .reservedBytes = reservedBytes_,
        .peakReservedBytes = peakReservedBytes_,
        .reservationRequests = reservationRequests_,
        .reservationFailures = reservationFailures_,
    };
}

bool PointMemoryBudget::resize(Reservation &reservation,
                               const std::uint64_t bytes)
{
    const std::scoped_lock lock(mutex_);
    const std::uint64_t previous =
        reservation.bytes_.load(std::memory_order_relaxed);
    if (bytes > previous) {
        const std::uint64_t increase = bytes - previous;
        const std::uint64_t budget =
            byteBudget_.load(std::memory_order_relaxed);
        if (increase > budget - reservedBytes_) {
            ++reservationFailures_;
            return false;
        }
        reservedBytes_ += increase;
        peakReservedBytes_ = std::max(peakReservedBytes_, reservedBytes_);
    } else {
        reservedBytes_ -= previous - bytes;
    }
    reservation.bytes_.store(bytes, std::memory_order_relaxed);
    return true;
}

void PointMemoryBudget::release(const std::uint64_t bytes) noexcept
{
    const std::scoped_lock lock(mutex_);
    reservedBytes_ = bytes <= reservedBytes_ ? reservedBytes_ - bytes : 0;
}

} // namespace pci
