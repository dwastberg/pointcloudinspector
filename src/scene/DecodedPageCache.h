#pragma once

#include "foundation/StrongId.h"
#include "scene/DecodedCacheMetrics.h"
#include "scene/PointCloudNode.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace pci {

using PointCloudSourceId = StrongId<struct PointCloudSourceIdTag>;

struct DecodedPageKey {
    PointCloudSourceId sourceId;
    PointCloudNodeId nodeId;

    bool operator==(const DecodedPageKey &) const = default;
};

struct DecodedPageKeyHash {
    [[nodiscard]] std::size_t
    operator()(const DecodedPageKey &key) const noexcept;
};

// One cache can serve every hierarchical source in a document. Payload leases
// remain shared_ptrs, so a current frame or decode completion can safely keep a
// page alive after its cache entry has become evictable.
class DecodedPageCache final {
public:
    explicit DecodedPageCache(
        std::uint64_t byteBudget = defaultDecodedCacheByteBudget);

    [[nodiscard]] PointCloudNodePayloadPtr find(PointCloudSourceId sourceId,
                                                PointCloudNodeId nodeId);
    // Inspect residency without recording a user-visible cache lookup. This
    // is intended for bounded asynchronous completion polling.
    [[nodiscard]] PointCloudNodePayloadPtr peek(PointCloudSourceId sourceId,
                                                PointCloudNodeId nodeId) const;
    [[nodiscard]] bool contains(PointCloudSourceId sourceId,
                                PointCloudNodeId nodeId) const;
    void insert(PointCloudSourceId sourceId,
                PointCloudNodePayloadPtr payload,
                std::span<const PointCloudNodeId> protectedNodes = {});
    void setPinned(PointCloudSourceId sourceId,
                   std::span<const PointCloudNodeId> nodes);
    void trim(PointCloudSourceId protectedSource = {},
              std::span<const PointCloudNodeId> protectedNodes = {});
    void removeSource(PointCloudSourceId sourceId);
    void clear();

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
    struct Entry {
        PointCloudNodePayloadPtr payload;
        std::uint64_t bytes = 0;
        std::uint64_t points = 0;
        std::uint64_t lastUsed = 0;
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

    void trimLocked(const std::unordered_set<DecodedPageKey, DecodedPageKeyHash>
                        &protectedKeys);
    void eraseLocked(
        std::unordered_map<DecodedPageKey, Entry, DecodedPageKeyHash>::iterator
            entry,
        bool eviction);
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
    std::unordered_map<DecodedPageKey, Entry, DecodedPageKeyHash> entries_;
    std::unordered_set<DecodedPageKey, DecodedPageKeyHash> pinned_;
    std::unordered_map<PointCloudSourceId, SourceState> sources_;
};

using DecodedPageCachePtr = std::shared_ptr<DecodedPageCache>;

} // namespace pci
