#pragma once

#include <pci/pointcloud/PointCloudDataSource.h>
#include <pci/pointcloud/PointIdentity.h>
#include <pci/pointcloud/SceneBlock.h>
#include <pci/runtime/PointMemoryBudget.h>

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace pci {

enum class RasterPointColorizeAvailability : std::uint8_t {
    Ready,
    Loading,
    Unsupported,
};

enum class RasterPointColorApplyOutcome : std::uint8_t {
    Applied,
    Stale,
};

struct RasterPointColorMetrics {
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t flatDisplacedColorBytes = 0;
    std::uint64_t retainedSourceRootBytes = 0;
    std::uint64_t retainedColoredRootBytes = 0;
};

struct PointColorHierarchyInstallation {
    PointCloudDataSourcePtr baseSource;
    PointCloudDataSourcePtr colorizedSource;
    PointCloudNodePayloadPtr sourceRootPayload;
    PointCloudNodePayloadPtr coloredRootPayload;
    std::uint64_t expectedRootPayloadRevision = 0;
    PointMemoryBudget::ReservationPtr rootStagingReservation;
};

struct PointColorFlatBlockIdentity {
    std::uint64_t id = 0;
    std::weak_ptr<const PointBlock> block;
    std::uint64_t pointCount = 0;
    std::uint64_t attributeCount = 0;
};

struct PointColorFlatInstallation {
    std::vector<PointColorFlatBlockIdentity> expectedBlocks;
    std::vector<SceneBlock> replacementBlocks;
    std::shared_ptr<std::vector<std::uint32_t>> displacedSourceColors;
    PointMemoryBudget::ReservationPtr colorReservation;
    PointMemoryBudget::ReservationPtr stagingReservation;
};

using PointColorInstallationData =
    std::variant<PointColorHierarchyInstallation, PointColorFlatInstallation>;

struct PointColorInstallation {
    PointCloudSourceId sourceId;
    PointColorInstallationData data;
};

using PointColorInstallationPtr = std::shared_ptr<PointColorInstallation>;

} // namespace pci
