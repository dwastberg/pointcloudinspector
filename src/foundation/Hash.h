#pragma once

#include <cstddef>

namespace pci {

[[nodiscard]] constexpr std::size_t
hashCombine(const std::size_t seed, const std::size_t value) noexcept
{
    return seed ^ (value + static_cast<std::size_t>(0x9e3779b9U) +
                   (seed << 6U) + (seed >> 2U));
}

} // namespace pci
