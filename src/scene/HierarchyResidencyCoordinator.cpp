#include "scene/HierarchyResidencyCoordinator.h"

#include "foundation/CheckedArithmetic.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pci {
HierarchyDecodeAdmission::Lease::Lease(
    std::shared_ptr<HierarchyDecodeAdmission> owner,
    const std::uint64_t estimatedBytes)
    : owner_(std::move(owner))
    , estimatedBytes_(estimatedBytes)
{
}

HierarchyDecodeAdmission::Lease::~Lease()
{
    release();
}

HierarchyDecodeAdmission::Lease::Lease(Lease &&other) noexcept
    : owner_(std::move(other.owner_))
    , estimatedBytes_(std::exchange(other.estimatedBytes_, 0))
{
}

HierarchyDecodeAdmission::Lease &
HierarchyDecodeAdmission::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        release();
        owner_ = std::move(other.owner_);
        estimatedBytes_ = std::exchange(other.estimatedBytes_, 0);
    }
    return *this;
}

void HierarchyDecodeAdmission::Lease::release() noexcept
{
    if (owner_) {
        owner_->release(estimatedBytes_);
        owner_.reset();
        estimatedBytes_ = 0;
    }
}

HierarchyDecodeAdmission::HierarchyDecodeAdmission(
    const std::size_t maximumConcurrentDecodes)
    : maximumConcurrentDecodes_(maximumConcurrentDecodes)
{
    if (maximumConcurrentDecodes == 0) {
        throw std::invalid_argument(
            "maximum concurrent hierarchy decodes must be positive");
    }
}

std::optional<HierarchyDecodeAdmission::Lease>
HierarchyDecodeAdmission::acquire(const std::stop_token workerStop,
                                  const std::stop_token requestStop,
                                  const std::uint64_t estimatedBytes)
{
    const auto waitStarted = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    if (workerStop.stop_requested() || requestStop.stop_requested()) {
        return std::nullopt;
    }
    if (nextTicket_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "hierarchy decode admission tickets are exhausted");
    }
    const std::uint64_t ticket = nextTicket_++;
    pending_.push_back(ticket);
    ++requests_;
    peakPending_ = std::max(peakPending_, pending_.size());
    const std::stop_callback requestCancelled(requestStop, [this] {
        ready_.notify_all();
    });
    const auto ready = [this, ticket, &requestStop] {
        return requestStop.stop_requested() ||
               (!pending_.empty() && pending_.front() == ticket &&
                active_ < maximumConcurrentDecodes_);
    };
    if (!ready_.wait(lock, workerStop, ready) || requestStop.stop_requested()) {
        erasePending(ticket);
        ++cancelled_;
        lock.unlock();
        ready_.notify_all();
        return std::nullopt;
    }

    pending_.pop_front();
    ++active_;
    ++admitted_;
    peakActive_ = std::max(peakActive_, active_);
    activeEstimatedBytes_ =
        saturatingAdd(activeEstimatedBytes_, estimatedBytes);
    peakActiveEstimatedBytes_ =
        std::max(peakActiveEstimatedBytes_, activeEstimatedBytes_);
    const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - waitStarted);
    totalWaitNanoseconds_ = saturatingAdd(
        totalWaitNanoseconds_, static_cast<std::uint64_t>(waited.count()));
    lock.unlock();
    ready_.notify_all();
    return Lease(shared_from_this(), estimatedBytes);
}

std::size_t HierarchyDecodeAdmission::maximumConcurrentDecodes() const noexcept
{
    return maximumConcurrentDecodes_;
}

std::size_t HierarchyDecodeAdmission::activeDecodeCount() const
{
    const std::scoped_lock lock(mutex_);
    return active_;
}

std::size_t HierarchyDecodeAdmission::pendingDecodeCount() const
{
    const std::scoped_lock lock(mutex_);
    return pending_.size();
}

HierarchyDecodeAdmissionMetrics HierarchyDecodeAdmission::metrics() const
{
    const std::scoped_lock lock(mutex_);
    return {
        .requests = requests_,
        .admitted = admitted_,
        .cancelled = cancelled_,
        .totalWaitNanoseconds = totalWaitNanoseconds_,
        .activeEstimatedBytes = activeEstimatedBytes_,
        .peakActiveEstimatedBytes = peakActiveEstimatedBytes_,
        .active = active_,
        .peakActive = peakActive_,
        .pending = pending_.size(),
        .peakPending = peakPending_,
    };
}

void HierarchyDecodeAdmission::release(
    const std::uint64_t estimatedBytes) noexcept
{
    {
        const std::scoped_lock lock(mutex_);
        if (active_ > 0) {
            --active_;
        }
        activeEstimatedBytes_ = estimatedBytes > activeEstimatedBytes_
                                    ? 0
                                    : activeEstimatedBytes_ - estimatedBytes;
    }
    ready_.notify_all();
}

void HierarchyDecodeAdmission::erasePending(const std::uint64_t ticket) noexcept
{
    const auto found = std::ranges::find(pending_, ticket);
    if (found != pending_.end()) {
        pending_.erase(found);
    }
}

HierarchyResidencyCoordinator::Participant::Participant(
    std::shared_ptr<HierarchyResidencyCoordinator> owner,
    const ParticipantId id)
    : owner_(std::move(owner))
    , id_(id)
{
}

HierarchyResidencyCoordinator::Participant::~Participant()
{
    owner_->unregisterParticipant(id_);
}

void HierarchyResidencyCoordinator::Participant::setActive(const bool active)
{
    owner_->setParticipantActive(id_, active);
}

void HierarchyResidencyCoordinator::Participant::setRetainedRootBytes(
    const std::uint64_t bytes)
{
    owner_->setParticipantRetainedRootBytes(id_, bytes);
}

std::uint64_t HierarchyResidencyCoordinator::Participant::byteBudget() const
{
    return owner_->participantByteBudget(id_);
}

std::optional<HierarchyResidencyCoordinator::DecodeLease>
HierarchyResidencyCoordinator::Participant::acquireDecode(
    const std::stop_token workerStop,
    const std::stop_token requestStop,
    const std::uint64_t estimatedBytes) const
{
    return owner_->acquireDecode(id_, workerStop, requestStop, estimatedBytes);
}

HierarchyResidencyCoordinator::HierarchyResidencyCoordinator(
    const std::uint64_t byteBudget,
    const std::size_t maximumConcurrentDecodes,
    HierarchyDecodeAdmissionPtr decodeAdmission,
    PointMemoryBudgetPtr memoryBudget)
    : memoryBudget_(memoryBudget
                        ? std::move(memoryBudget)
                        : std::make_shared<PointMemoryBudget>(byteBudget))
    , decodeAdmission_(decodeAdmission
                           ? std::move(decodeAdmission)
                           : std::make_shared<HierarchyDecodeAdmission>(
                                 maximumConcurrentDecodes))
{
    if (byteBudget == 0) {
        throw std::invalid_argument(
            "hierarchy residency budget must be positive");
    }
    if (memoryBudget_->byteBudget() != byteBudget) {
        throw std::invalid_argument(
            "shared point-memory budget has a different byte limit");
    }
    if (maximumConcurrentDecodes !=
        decodeAdmission_->maximumConcurrentDecodes()) {
        throw std::invalid_argument(
            "shared hierarchy decode admission has a different limit");
    }
}

HierarchyResidencyCoordinator::ParticipantPtr
HierarchyResidencyCoordinator::registerParticipant(
    const std::uint64_t retainedRootBytes, const bool active)
{
    return registerParticipant(retainedRootBytes, active, true);
}

HierarchyResidencyCoordinator::ParticipantPtr
HierarchyResidencyCoordinator::registerTransientDecode()
{
    return registerParticipant(0, true, false);
}

HierarchyResidencyCoordinator::ParticipantPtr
HierarchyResidencyCoordinator::registerParticipant(
    const std::uint64_t retainedRootBytes,
    const bool active,
    const bool sharesResidencyBudget)
{
    ParticipantId id = 0;
    {
        const std::scoped_lock lock(mutex_);
        if (nextParticipantId_ == std::numeric_limits<ParticipantId>::max()) {
            throw std::overflow_error(
                "hierarchy residency participant ids are exhausted");
        }
        id = nextParticipantId_++;
        participants_.emplace(
            id,
            ParticipantState{
                .retainedRootBytes = retainedRootBytes,
                .active = active,
                .sharesResidencyBudget = sharesResidencyBudget,
            });
    }
    return std::shared_ptr<Participant>(
        new Participant(shared_from_this(), id));
}

std::uint64_t HierarchyResidencyCoordinator::byteBudget() const noexcept
{
    return memoryBudget_->byteBudget();
}

std::uint64_t HierarchyResidencyCoordinator::availableResidencyBytes() const
{
    return memoryBudget_->availableBytes();
}

std::uint64_t HierarchyResidencyCoordinator::retainedRootBytes() const
{
    const std::scoped_lock lock(mutex_);
    std::uint64_t result = 0;
    for (const auto &[id, participant] : participants_) {
        static_cast<void>(id);
        if (participant.sharesResidencyBudget) {
            result = saturatingAdd(result, participant.retainedRootBytes);
        }
    }
    return result;
}

std::size_t
HierarchyResidencyCoordinator::maximumConcurrentDecodes() const noexcept
{
    return decodeAdmission_->maximumConcurrentDecodes();
}

std::size_t HierarchyResidencyCoordinator::activeDecodeCount() const
{
    return decodeAdmission_->activeDecodeCount();
}

std::size_t HierarchyResidencyCoordinator::pendingDecodeCount() const
{
    return decodeAdmission_->pendingDecodeCount();
}

const HierarchyDecodeAdmissionPtr &
HierarchyResidencyCoordinator::decodeAdmission() const noexcept
{
    return decodeAdmission_;
}

void HierarchyResidencyCoordinator::unregisterParticipant(
    const ParticipantId id) noexcept
{
    const std::scoped_lock lock(mutex_);
    participants_.erase(id);
}

void HierarchyResidencyCoordinator::setParticipantActive(const ParticipantId id,
                                                         const bool active)
{
    const std::scoped_lock lock(mutex_);
    const auto found = participants_.find(id);
    if (found == participants_.end()) {
        throw std::logic_error(
            "hierarchy residency participant is no longer registered");
    }
    found->second.active = active;
}

void HierarchyResidencyCoordinator::setParticipantRetainedRootBytes(
    const ParticipantId id, const std::uint64_t bytes)
{
    const std::scoped_lock lock(mutex_);
    const auto found = participants_.find(id);
    if (found == participants_.end()) {
        throw std::logic_error(
            "hierarchy residency participant is no longer registered");
    }
    found->second.retainedRootBytes = bytes;
}

std::uint64_t HierarchyResidencyCoordinator::participantByteBudget(
    const ParticipantId id) const
{
    const std::scoped_lock lock(mutex_);
    const auto requested = participants_.find(id);
    if (requested == participants_.end()) {
        throw std::logic_error(
            "hierarchy residency participant is no longer registered");
    }
    if (!requested->second.sharesResidencyBudget) {
        return 1;
    }

    std::uint64_t retainedBytes = 0;
    std::size_t activeCount = 0;
    std::size_t activeRank = 0;
    for (const auto &[participantId, participant] : participants_) {
        if (!participant.sharesResidencyBudget) {
            continue;
        }
        retainedBytes =
            saturatingAdd(retainedBytes, participant.retainedRootBytes);
        if (participant.active) {
            if (participantId < id) {
                ++activeRank;
            }
            ++activeCount;
        }
    }

    std::uint64_t allocation = requested->second.retainedRootBytes;
    const std::uint64_t residencyBudget = memoryBudget_->availableBytes();
    if (requested->second.active && activeCount > 0 &&
        residencyBudget > retainedBytes) {
        const std::uint64_t discretionary = residencyBudget - retainedBytes;
        const std::uint64_t count = static_cast<std::uint64_t>(activeCount);
        allocation = saturatingAdd(allocation, discretionary / count);
        if (activeRank < discretionary % count) {
            allocation = saturatingAdd(allocation, std::uint64_t{1});
        }
    }
    return std::max<std::uint64_t>(allocation, 1);
}

std::optional<HierarchyResidencyCoordinator::DecodeLease>
HierarchyResidencyCoordinator::acquireDecode(const ParticipantId id,
                                             const std::stop_token workerStop,
                                             const std::stop_token requestStop,
                                             const std::uint64_t estimatedBytes)
{
    {
        const std::scoped_lock lock(mutex_);
        const auto participant = participants_.find(id);
        if (participant == participants_.end() || !participant->second.active) {
            return std::nullopt;
        }
    }
    auto lease =
        decodeAdmission_->acquire(workerStop, requestStop, estimatedBytes);
    if (!lease) {
        return std::nullopt;
    }
    const std::scoped_lock lock(mutex_);
    const auto participant = participants_.find(id);
    if (participant == participants_.end() || !participant->second.active) {
        return std::nullopt;
    }
    return lease;
}

} // namespace pci
