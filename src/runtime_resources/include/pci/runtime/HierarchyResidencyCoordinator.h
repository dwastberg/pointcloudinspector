#pragma once

#include <pci/runtime/PointMemoryBudget.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>

namespace pci {

struct HierarchyDecodeAdmissionMetrics {
    std::uint64_t requests = 0;
    std::uint64_t admitted = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t totalWaitNanoseconds = 0;
    std::uint64_t activeEstimatedBytes = 0;
    std::uint64_t peakActiveEstimatedBytes = 0;
    std::size_t active = 0;
    std::size_t peakActive = 0;
    std::size_t pending = 0;
    std::size_t peakPending = 0;
};

class HierarchyDecodeAdmission final
    : public std::enable_shared_from_this<HierarchyDecodeAdmission> {
public:
    static constexpr std::size_t defaultMaximumConcurrentDecodes = 2;

    class Lease final {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;

    private:
        friend class HierarchyDecodeAdmission;
        Lease(std::shared_ptr<HierarchyDecodeAdmission> owner,
              std::uint64_t estimatedBytes);
        void release() noexcept;

        std::shared_ptr<HierarchyDecodeAdmission> owner_;
        std::uint64_t estimatedBytes_ = 0;
    };

    explicit HierarchyDecodeAdmission(
        std::size_t maximumConcurrentDecodes = defaultMaximumConcurrentDecodes);

    [[nodiscard]] std::optional<Lease>
    acquire(std::stop_token workerStop,
            std::stop_token requestStop,
            std::uint64_t estimatedBytes = 0);
    [[nodiscard]] std::size_t maximumConcurrentDecodes() const noexcept;
    [[nodiscard]] std::size_t activeDecodeCount() const;
    [[nodiscard]] std::size_t pendingDecodeCount() const;
    [[nodiscard]] HierarchyDecodeAdmissionMetrics metrics() const;

private:
    void release(std::uint64_t estimatedBytes) noexcept;
    void erasePending(std::uint64_t ticket) noexcept;

    const std::size_t maximumConcurrentDecodes_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::uint64_t> pending_;
    std::uint64_t nextTicket_ = 1;
    std::size_t active_ = 0;
    std::size_t peakActive_ = 0;
    std::size_t peakPending_ = 0;
    std::uint64_t requests_ = 0;
    std::uint64_t admitted_ = 0;
    std::uint64_t cancelled_ = 0;
    std::uint64_t totalWaitNanoseconds_ = 0;
    std::uint64_t activeEstimatedBytes_ = 0;
    std::uint64_t peakActiveEstimatedBytes_ = 0;
};

using HierarchyDecodeAdmissionPtr = std::shared_ptr<HierarchyDecodeAdmission>;

class HierarchyResidencyCoordinator final
    : public std::enable_shared_from_this<HierarchyResidencyCoordinator> {
public:
    using ParticipantId = std::uint64_t;
    using DecodeLease = HierarchyDecodeAdmission::Lease;

    static constexpr std::uint64_t defaultByteBudget =
        std::uint64_t{512} * 1024 * 1024;
    static constexpr std::size_t defaultMaximumConcurrentDecodes =
        HierarchyDecodeAdmission::defaultMaximumConcurrentDecodes;

    class Participant final {
    public:
        ~Participant();

        Participant(const Participant &) = delete;
        Participant &operator=(const Participant &) = delete;

        void setActive(bool active);
        void setRetainedRootBytes(std::uint64_t bytes);
        void setRootBytes(std::uint64_t retained,
                          std::uint64_t reserved) noexcept;
        [[nodiscard]] std::uint64_t byteBudget() const;
        [[nodiscard]] std::optional<DecodeLease>
        acquireDecode(std::stop_token workerStop,
                      std::stop_token requestStop,
                      std::uint64_t estimatedBytes = 0) const;

    private:
        friend class HierarchyResidencyCoordinator;
        Participant(std::shared_ptr<HierarchyResidencyCoordinator> owner,
                    ParticipantId id);

        std::shared_ptr<HierarchyResidencyCoordinator> owner_;
        ParticipantId id_ = 0;
    };

    using ParticipantPtr = std::shared_ptr<Participant>;

    explicit HierarchyResidencyCoordinator(
        std::uint64_t byteBudget = defaultByteBudget,
        std::size_t maximumConcurrentDecodes = defaultMaximumConcurrentDecodes,
        HierarchyDecodeAdmissionPtr decodeAdmission = {},
        PointMemoryBudgetPtr memoryBudget = {});

    [[nodiscard]] ParticipantPtr
    registerParticipant(std::uint64_t retainedRootBytes,
                        bool active = true,
                        std::uint64_t reservedRootBytes = 0);
    // Import-time root queries share decode admission but do not consume a
    // persistent cache allocation until their scene joins the document.
    [[nodiscard]] ParticipantPtr registerTransientDecode();

    [[nodiscard]] std::uint64_t byteBudget() const noexcept;
    [[nodiscard]] std::uint64_t availableResidencyBytes() const;
    [[nodiscard]] std::uint64_t retainedRootBytes() const;
    [[nodiscard]] std::size_t maximumConcurrentDecodes() const noexcept;
    [[nodiscard]] std::size_t activeDecodeCount() const;
    [[nodiscard]] std::size_t pendingDecodeCount() const;
    [[nodiscard]] const HierarchyDecodeAdmissionPtr &
    decodeAdmission() const noexcept;

private:
    struct ParticipantState {
        std::uint64_t retainedRootBytes = 0;
        std::uint64_t reservedRootBytes = 0;
        bool active = true;
        bool sharesResidencyBudget = true;
    };

    void unregisterParticipant(ParticipantId id) noexcept;
    [[nodiscard]] ParticipantPtr
    registerParticipant(std::uint64_t retainedRootBytes,
                        bool active,
                        bool sharesResidencyBudget,
                        std::uint64_t reservedRootBytes);
    void setParticipantActive(ParticipantId id, bool active);
    void setParticipantRetainedRootBytes(ParticipantId id, std::uint64_t bytes);
    [[nodiscard]] std::uint64_t participantByteBudget(ParticipantId id) const;
    [[nodiscard]] std::optional<DecodeLease>
    acquireDecode(ParticipantId id,
                  std::stop_token workerStop,
                  std::stop_token requestStop,
                  std::uint64_t estimatedBytes);

    PointMemoryBudgetPtr memoryBudget_;
    HierarchyDecodeAdmissionPtr decodeAdmission_;
    mutable std::mutex mutex_;
    std::unordered_map<ParticipantId, ParticipantState> participants_;
    ParticipantId nextParticipantId_ = 1;
};

using HierarchyResidencyCoordinatorPtr =
    std::shared_ptr<HierarchyResidencyCoordinator>;

} // namespace pci
