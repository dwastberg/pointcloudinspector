#pragma once

#include <pci/pointcloud/PointColorPolicy.h>

#include <pci/pointcloud/PointBlock.h>

#include <cstdint>
#include <vector>

namespace pci {

struct PointDatasetRuntimeBlockSnapshot {
    std::uint64_t id = 0;
    PointBlockPtr block;
    Vec3d center;
    double radius = 0.0;
    std::uint64_t pointCount = 0;
    std::uint64_t byteSize = 0;
};

// Immutable renderer-facing metadata captured under one scene lock. Flat
// snapshots may retain scene-owned immutable blocks. Hierarchical snapshots
// deliberately contain no decoded payload handles; those are leased only by a
// current frame or in-flight operation so cache eviction remains effective.
struct PointDatasetRuntimeSnapshot {
    std::uint64_t revision = 0;
    std::uint64_t rootPayloadRevision = 0;
    bool hierarchical = false;
    bool loadingComplete = false;
    std::uint64_t sourcePointCount = 0;
    std::uint64_t retainedFlatPointCount = 0;
    std::uint64_t decodedResidentPointCount = 0;
    std::uint64_t decodedResidentBytes = 0;
    Bounds3d bounds;
    PointCloudScalarRanges scalarRanges;
    std::uint16_t intensityMinimum = 0;
    std::uint16_t intensityMaximum = 0;
    std::vector<PointDatasetRuntimeBlockSnapshot> flatBlocks;
};

} // namespace pci
