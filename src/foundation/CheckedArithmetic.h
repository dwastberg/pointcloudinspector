#pragma once

#include <concepts>
#include <limits>
#include <optional>

namespace pci {

template <std::unsigned_integral T>
[[nodiscard]] constexpr T saturatingAdd(const T left, const T right) noexcept
{
    return right > std::numeric_limits<T>::max() - left
               ? std::numeric_limits<T>::max()
               : left + right;
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr T saturatingMultiply(const T left,
                                             const T right) noexcept
{
    return left != 0 && right > std::numeric_limits<T>::max() / left
               ? std::numeric_limits<T>::max()
               : left * right;
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr std::optional<T> checkedAdd(const T left,
                                                    const T right) noexcept
{
    if (right > std::numeric_limits<T>::max() - left) {
        return std::nullopt;
    }
    return left + right;
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr std::optional<T> checkedMultiply(const T left,
                                                         const T right) noexcept
{
    if (left != 0 && right > std::numeric_limits<T>::max() / left) {
        return std::nullopt;
    }
    return left * right;
}

} // namespace pci
