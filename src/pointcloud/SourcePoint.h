#pragma once

#include <array>
#include <cstdint>

namespace pci {

struct SourcePoint {
    std::array<double, 3> position{};
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t intensity = 0;
    std::uint8_t classification = 0;
    std::uint8_t returnNumber = 0;
    std::uint8_t numberOfReturns = 0;
    std::uint64_t sourceOrdinal = 0;
};

} // namespace pci
