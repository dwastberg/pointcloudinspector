#pragma once

#include <pci/pointcloud/PointCloudDataSource.h>
#include <pci/pointcloud/PointIdentity.h>
#include <pci/pointcloud/SceneBlock.h>

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace pci {

struct RasterColorizeHierarchicalTarget {
    PointCloudDataSourcePtr baseSource;
    std::vector<PointCloudStoredNode> storedNodes;
    PointCloudNodePayloadPtr sourceRootPayload;
    std::uint64_t expectedRootPayloadRevision = 0;
};

struct RasterColorizeFlatTarget {
    std::vector<SceneBlock> blocks;
    std::shared_ptr<const std::vector<std::uint32_t>> sourceColors;
};

using RasterColorizeTargetData =
    std::variant<RasterColorizeHierarchicalTarget, RasterColorizeFlatTarget>;

struct RasterColorizeTargetSnapshot {
    PointCloudSourceId sourceId;
    RasterColorizeTargetData data;
};

struct RasterColorizeRange {
    PointCloudNodeId node;
    std::uint64_t offset = 0;
    std::uint32_t count = 0;
};

} // namespace pci
