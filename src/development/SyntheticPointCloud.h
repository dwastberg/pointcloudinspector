#pragma once

#include "pointcloud/GpuPoint.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pci {

[[nodiscard]] GpuPoint generatePoint(std::uint64_t index);

std::vector<GpuPoint> generatePointChunk(std::uint64_t firstIndex,
                                         std::size_t count);

} // namespace pci
