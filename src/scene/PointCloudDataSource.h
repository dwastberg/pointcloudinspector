#pragma once

#include "pointcloud/PointColorPolicy.h"
#include "scene/PointCloudNode.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace pci {

struct PointCloudDataSourceMetrics {
    std::uint64_t requests = 0;
    std::uint64_t completed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t estimatedDecodedBytesRequested = 0;
    std::uint64_t decodedBytesProduced = 0;
    std::uint64_t sourcePointsVisited = 0;
    std::uint64_t decodedPointsProduced = 0;
    std::uint64_t totalQueryNanoseconds = 0;
    // PDAL's public COPC/EPT readers do not expose HTTP/file bytes fetched.
    std::uint64_t fetchedBytes = 0;
    bool fetchedBytesKnown = false;
};

struct PointCloudFullDetailInfo {
    std::vector<PointCloudNodeId> leafNodes;
    std::uint64_t pointCount = 0;
    std::uint64_t decodedBytes = 0;
    std::uint64_t gpuBytes = 0;
};

struct PointCloudStorageMetrics {
    std::uint64_t persistentBytes = 0;
    bool localPersistent = false;
    bool committed = false;
    bool reused = false;
};

struct PointCloudStoredNode {
    PointCloudNodeId id;
    Bounds3d bounds;
    std::uint64_t pointCount = 0;
    std::uint64_t localityKey = 0;
};

class PointCloudDataSourceCancelled final : public std::runtime_error {
public:
    PointCloudDataSourceCancelled()
        : std::runtime_error("point-cloud node request cancelled")
    {
    }
};

class PointCloudDataSource {
public:
    virtual ~PointCloudDataSource() = default;

    [[nodiscard]] virtual PointCloudNode rootNode() const = 0;
    [[nodiscard]] virtual PointCloudNode node(PointCloudNodeId id) const = 0;
    [[nodiscard]] virtual PointCloudNodePayloadPtr
    loadNode(PointCloudNodeId id, std::stop_token stopToken) const = 0;
    [[nodiscard]] virtual PointCloudDataSourceMetrics metrics() const
    {
        return {};
    }
    [[nodiscard]] virtual PointCloudStorageMetrics storageMetrics() const
    {
        return {};
    }
    // Complete source-domain statistics, when known independently of decoded
    // cache residency. Render normalization must not infer these from the
    // currently resident hierarchy cut.
    [[nodiscard]] virtual PointCloudScalarRanges scalarRanges() const
    {
        return {};
    }
    // Sources that can enumerate a finite, exact leaf working set expose it
    // here. Native query hierarchies may remain unbounded or remote and return
    // no value; the renderer then keeps normal screen-space LOD behavior.
    [[nodiscard]] virtual std::optional<PointCloudFullDetailInfo>
    fullDetailInfo() const
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<std::vector<PointCloudStoredNode>>
    storedNodeIndex() const
    {
        return std::nullopt;
    }
};

using PointCloudDataSourcePtr = std::shared_ptr<PointCloudDataSource>;

} // namespace pci
