#pragma once

#include <pci/pointcloud/PointCloudNode.h>
#include <pci/pointcloud/PointIdentity.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace pci {

// Immutable, effect-free view of the point data that is already available to
// one frame. Implementations may synchronize individual calls, but metadata
// lookup and payload acquisition must never schedule I/O, change cache
// metrics/recency, evict data, or expose a mutable runtime handle.
class PointResidencyView {
public:
    virtual ~PointResidencyView() = default;

    [[nodiscard]] virtual PointCloudSourceId sourceId() const noexcept = 0;
    // Changes when node metadata and resident payloads may represent a
    // different source-root/content installation. Ordinary cache admission
    // and eviction do not change this revision.
    [[nodiscard]] virtual std::uint64_t contentRevision() const noexcept = 0;
    // Sources may return a conservative descriptor while hierarchy discovery
    // is incomplete; an undiscovered node must not be treated as absent.
    [[nodiscard]] virtual PointCloudNode node(PointCloudNodeId id) const = 0;
    // Acquires a lease only when the payload is already resident.
    [[nodiscard]] virtual PointCloudNodePayloadPtr
    acquire(PointCloudNodeId id) const = 0;
    [[nodiscard]] virtual std::string_view error() const noexcept = 0;
};

using PointResidencyViewPtr = std::shared_ptr<const PointResidencyView>;

} // namespace pci
