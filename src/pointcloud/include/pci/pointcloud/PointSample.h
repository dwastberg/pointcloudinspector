#pragma once

#include <pci/foundation/Vec3d.h>
#include <pci/pointcloud/PointAttributes.h>

#include <cstdint>

namespace pci {

// Adapter-neutral decoded point used while constructing immutable blocks.
struct PointSample {
    Vec3d position;
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t packedAttributes = 0;
    std::uint32_t packedProperties = 0;
    PointAttributes attributes;
};

} // namespace pci
