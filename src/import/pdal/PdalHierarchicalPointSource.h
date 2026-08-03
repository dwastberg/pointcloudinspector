#pragma once

#include "pointcloud/PointCloudMetadata.h"
#include "scene/PointCloudDataSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pci {

class PdalHierarchicalPointSource final : public PointCloudDataSource {
public:
    static constexpr std::size_t defaultPointsPerNode = 65'536;

    PdalHierarchicalPointSource(
        PointCloudMetadata metadata,
        std::uint64_t maximumPoints,
        std::size_t pointsPerNode = defaultPointsPerNode);

    [[nodiscard]] PointCloudNode rootNode() const override;
    [[nodiscard]] PointCloudNode node(PointCloudNodeId id) const override;
    [[nodiscard]] PointCloudNodePayloadPtr
    loadNode(PointCloudNodeId id, std::stop_token stopToken) const override;
    [[nodiscard]] PointCloudDataSourceMetrics metrics() const override;

    [[nodiscard]] std::uint8_t maximumLevel() const noexcept;
    [[nodiscard]] double resolution(PointCloudNodeId id) const;

private:
    [[nodiscard]] bool contains(PointCloudNodeId id,
                                const Vec3d &position) const noexcept;

    PointCloudMetadata metadata_;
    std::uint64_t selectedPointCount_ = 0;
    std::size_t pointsPerNode_ = defaultPointsPerNode;
    std::uint8_t maximumLevel_ = 0;
    mutable std::atomic_uint64_t requests_ = 0;
    mutable std::atomic_uint64_t completed_ = 0;
    mutable std::atomic_uint64_t cancelled_ = 0;
    mutable std::atomic_uint64_t failed_ = 0;
    mutable std::atomic_uint64_t estimatedDecodedBytesRequested_ = 0;
    mutable std::atomic_uint64_t decodedBytesProduced_ = 0;
    mutable std::atomic_uint64_t sourcePointsVisited_ = 0;
    mutable std::atomic_uint64_t decodedPointsProduced_ = 0;
    mutable std::atomic_uint64_t totalQueryNanoseconds_ = 0;
};

} // namespace pci
