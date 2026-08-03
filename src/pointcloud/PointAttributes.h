#pragma once

#include <cstdint>

namespace pci {

struct PointAttributes {
    std::uint16_t intensity = 0;
    std::uint8_t classification = 0;
    std::uint8_t returnNumber = 0;
    std::uint8_t numberOfReturns = 0;

    bool operator==(const PointAttributes &) const = default;
};

static_assert(sizeof(PointAttributes) <= 8);

} // namespace pci
