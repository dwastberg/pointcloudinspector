#pragma once

#include <cstdint>

namespace pci {

struct GpuPoint {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t z;
    std::uint16_t attributes;
    std::uint32_t rgba;
    std::uint32_t packedProperties;

    bool operator==(const GpuPoint &) const = default;
};

static_assert(sizeof(GpuPoint) == 16);

} // namespace pci
