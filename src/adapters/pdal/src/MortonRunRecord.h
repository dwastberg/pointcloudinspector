#pragma once

#include <pci/pointcloud/BlockPartitioner.h>

#include <cstdint>
#include <type_traits>

namespace pci::local_index {

struct MortonRunRecord {
    std::uint64_t morton = 0;
    std::uint64_t ordinal = 0;
    PointSample sample;
};

static_assert(std::is_trivially_copyable_v<MortonRunRecord>);

} // namespace pci::local_index
