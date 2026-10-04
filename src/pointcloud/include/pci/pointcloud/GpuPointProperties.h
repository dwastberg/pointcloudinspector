#pragma once

#include <cstdint>

namespace pci {

constexpr std::uint32_t
packGpuPointProperties(const std::uint16_t intensity,
                       const std::uint8_t returnNumber,
                       const std::uint8_t numberOfReturns) noexcept
{
    return static_cast<std::uint32_t>(intensity) |
           (static_cast<std::uint32_t>(returnNumber) << 16U) |
           (static_cast<std::uint32_t>(numberOfReturns) << 24U);
}

constexpr std::uint16_t
gpuPointIntensity(const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint16_t>(packedProperties & 0xffffU);
}

constexpr std::uint8_t
gpuPointReturnNumber(const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint8_t>((packedProperties >> 16U) & 0xffU);
}

constexpr std::uint8_t
gpuPointNumberOfReturns(const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint8_t>((packedProperties >> 24U) & 0xffU);
}

} // namespace pci
