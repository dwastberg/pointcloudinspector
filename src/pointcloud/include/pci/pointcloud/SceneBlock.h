#pragma once

#include <pci/pointcloud/PointBlock.h>

#include <cstdint>

namespace pci {

struct SceneBlock {
    std::uint64_t id = 0;
    PointBlockPtr block;
};

} // namespace pci
