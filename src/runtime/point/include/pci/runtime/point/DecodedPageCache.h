#pragma once

#include <pci/runtime/point/DecodedCacheMetrics.h>

#include <pci/pointcloud/PointCloudNode.h>
#include <pci/pointcloud/PointIdentity.h>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace pci {

struct DecodedPageKey {
    PointCloudSourceId sourceId;
    PointCloudNodeId nodeId;

    bool operator==(const DecodedPageKey &) const = default;
};

struct DecodedPageKeyHash {
    [[nodiscard]] std::size_t
    operator()(const DecodedPageKey &key) const noexcept;
};

namespace detail {
// Cache-local allocator state. Faults are armed only by the test accessor;
// production allocations still use the standard allocator.
struct DecodedCacheAllocationControl {
    std::atomic_size_t failAt = 0;
    std::atomic_size_t attempts = 0;
    std::atomic_size_t outstanding = 0;
};
template <class T> class DecodedCacheAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    std::shared_ptr<DecodedCacheAllocationControl> control;
    DecodedCacheAllocator() = default;
    explicit DecodedCacheAllocator(
        std::shared_ptr<DecodedCacheAllocationControl> value)
        : control(std::move(value))
    {
    }
    template <class U>
    DecodedCacheAllocator(const DecodedCacheAllocator<U> &other) noexcept
        : control(other.control)
    {
    }
    [[nodiscard]] T *allocate(std::size_t count)
    {
        if (control && ++control->attempts == control->failAt)
            throw std::bad_alloc();
        auto *result = std::allocator<T>{}.allocate(count);
        if (control)
            ++control->outstanding;
        return result;
    }
    void deallocate(T *value, std::size_t count) noexcept
    {
        if (control)
            --control->outstanding;
        std::allocator<T>{}.deallocate(value, count);
    }
    template <class U>
    bool operator==(const DecodedCacheAllocator<U> &other) const noexcept
    {
        return control == other.control;
    }
};
} // namespace detail

// One cache can serve every hierarchical source in a document. Payload leases
// remain shared_ptrs, so a current frame or decode completion can safely keep a
// page alive after its cache entry has become evictable.
class DecodedPageCache final {
public:
    class RootReplacement final {
    public:
        ~RootReplacement();
        void commit() noexcept;

    private:
        friend class DecodedPageCache;
        struct State;
        explicit RootReplacement(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
    explicit DecodedPageCache(
        std::uint64_t byteBudget = defaultDecodedCacheByteBudget);
    [[nodiscard]] std::shared_ptr<DecodedPageCache> clone() const;

    [[nodiscard]] PointCloudNodePayloadPtr find(PointCloudSourceId sourceId,
                                                PointCloudNodeId nodeId);
    // Inspect residency without recording a user-visible cache lookup. This
    // is intended for bounded asynchronous completion polling.
    [[nodiscard]] PointCloudNodePayloadPtr peek(PointCloudSourceId sourceId,
                                                PointCloudNodeId nodeId) const;
    [[nodiscard]] bool contains(PointCloudSourceId sourceId,
                                PointCloudNodeId nodeId) const;
    // Applies the outcome of an earlier read-only residency probe without
    // retrieving the payload again. This preserves lookup metrics and LRU
    // policy when frame planning is separated from effect execution.
    void applyLookupEffect(PointCloudSourceId sourceId,
                           PointCloudNodeId nodeId,
                           bool resident);
    void insert(PointCloudSourceId sourceId,
                PointCloudNodePayloadPtr payload,
                std::span<const PointCloudNodeId> protectedNodes = {});
    void setPinned(PointCloudSourceId sourceId,
                   std::span<const PointCloudNodeId> nodes);
    void trim(PointCloudSourceId protectedSource = {},
              std::span<const PointCloudNodeId> protectedNodes = {});
    void removeSource(PointCloudSourceId sourceId);
    // Atomically replaces one source's residency with a new root. All
    // fallible allocations are completed against private copies first.
    void replaceSourceRoot(PointCloudSourceId sourceId,
                           PointCloudNodePayloadPtr root,
                           std::span<const PointCloudNodeId> pins);
    // Holds the cache lock through commit/abandon, so unrelated concurrent
    // decodes cannot be overwritten by the privately prepared map copies.
    [[nodiscard]] std::unique_ptr<RootReplacement>
    prepareSourceRoot(PointCloudSourceId sourceId,
                      PointCloudNodePayloadPtr root,
                      std::span<const PointCloudNodeId> pins);
    void clear();

    void setFrameProtection(std::span<const DecodedPageKey> keys);
    void setByteBudget(std::uint64_t byteBudget);
    [[nodiscard]] std::uint64_t byteBudget() const;
    [[nodiscard]] std::uint64_t residentBytes() const;
    [[nodiscard]] std::uint64_t residentPoints() const;
    [[nodiscard]] std::uint64_t
    residentBytes(PointCloudSourceId sourceId) const;
    [[nodiscard]] std::uint64_t
    residentPoints(PointCloudSourceId sourceId) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] DecodedCacheMetrics metrics() const;
    [[nodiscard]] DecodedCacheMetrics
    metrics(PointCloudSourceId sourceId) const;

private:
    friend struct DecodedPageCacheTestAccess;
    using Recency = std::list<DecodedPageKey,
                              detail::DecodedCacheAllocator<DecodedPageKey>>;
    struct Entry {
        PointCloudNodePayloadPtr payload;
        std::uint64_t bytes = 0;
        std::uint64_t points = 0;
        std::uint64_t lastUsed = 0;
        Recency::iterator recency;
    };

    struct SourceState {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t insertions = 0;
        std::uint64_t replacements = 0;
        std::uint64_t evictions = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t peakResidentBytes = 0;
        std::uint64_t residentPoints = 0;
    };

    template <class T> using Allocator = detail::DecodedCacheAllocator<T>;
    using Entries =
        std::unordered_map<DecodedPageKey,
                           Entry,
                           DecodedPageKeyHash,
                           std::equal_to<DecodedPageKey>,
                           Allocator<std::pair<const DecodedPageKey, Entry>>>;
    using Keys = std::unordered_set<DecodedPageKey,
                                    DecodedPageKeyHash,
                                    std::equal_to<DecodedPageKey>,
                                    Allocator<DecodedPageKey>>;
    using Sources = std::unordered_map<
        PointCloudSourceId,
        SourceState,
        std::hash<PointCloudSourceId>,
        std::equal_to<PointCloudSourceId>,
        Allocator<std::pair<const PointCloudSourceId, SourceState>>>;
    void trimLocked(const Keys &protectedKeys);
    void eraseLocked(Entries::iterator entry, bool eviction);
    [[nodiscard]] DecodedCacheMetrics
    metricsLocked(PointCloudSourceId sourceId) const;

    mutable std::mutex mutex_;
    std::uint64_t byteBudget_;
    std::uint64_t residentBytes_ = 0;
    std::uint64_t residentPoints_ = 0;
    std::uint64_t peakResidentBytes_ = 0;
    std::uint64_t clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t insertions_ = 0;
    std::uint64_t replacements_ = 0;
    std::uint64_t evictions_ = 0;
    std::shared_ptr<detail::DecodedCacheAllocationControl> allocation_ =
        std::make_shared<detail::DecodedCacheAllocationControl>();
    std::size_t lastTrimInspections_ = 0;
    Recency recency_{Allocator<DecodedPageKey>{allocation_}};
    Entries entries_{
        Allocator<std::pair<const DecodedPageKey, Entry>>{allocation_}};
    Keys pinned_{Allocator<DecodedPageKey>{allocation_}};
    Keys frameProtection_{Allocator<DecodedPageKey>{allocation_}};
    Sources sources_{
        Allocator<std::pair<const PointCloudSourceId, SourceState>>{
            allocation_}};
};

using DecodedPageCachePtr = std::shared_ptr<DecodedPageCache>;

} // namespace pci
