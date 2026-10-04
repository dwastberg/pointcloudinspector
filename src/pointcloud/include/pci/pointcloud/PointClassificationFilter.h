#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pci {

inline constexpr std::size_t pointClassificationCount = 256;
inline constexpr std::size_t pointClassificationMaskWordCount =
    pointClassificationCount / 32;

class PointClassificationFilter {
public:
    constexpr PointClassificationFilter() noexcept
    {
        words_.fill(std::numeric_limits<std::uint32_t>::max());
    }

    [[nodiscard]] static constexpr PointClassificationFilter
    noneVisible() noexcept
    {
        PointClassificationFilter result;
        result.words_.fill(0);
        return result;
    }

    [[nodiscard]] constexpr bool
    isVisible(const std::uint8_t classification) const noexcept
    {
        const std::size_t word = classification / 32U;
        const std::uint32_t bit = std::uint32_t{1} << (classification % 32U);
        return (words_[word] & bit) != 0;
    }

    constexpr void setVisible(const std::uint8_t classification,
                              const bool visible) noexcept
    {
        const std::size_t word = classification / 32U;
        const std::uint32_t bit = std::uint32_t{1} << (classification % 32U);
        if (visible) {
            words_[word] |= bit;
        } else {
            words_[word] &= ~bit;
        }
    }

    constexpr void setAllVisible(const bool visible) noexcept
    {
        words_.fill(visible ? std::numeric_limits<std::uint32_t>::max() : 0);
    }

    constexpr void include(const PointClassificationFilter &other) noexcept
    {
        for (std::size_t index = 0; index < words_.size(); ++index) {
            words_[index] |= other.words_[index];
        }
    }

    [[nodiscard]] constexpr std::size_t visibleCount() const noexcept
    {
        std::size_t count = 0;
        for (const std::uint32_t word : words_) {
            count += static_cast<std::size_t>(std::popcount(word));
        }
        return count;
    }

    [[nodiscard]] constexpr bool allVisible() const noexcept
    {
        return visibleCount() == pointClassificationCount;
    }

    [[nodiscard]] constexpr bool anyVisible() const noexcept
    {
        for (const std::uint32_t word : words_) {
            if (word != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr const std::array<std::uint32_t,
                                             pointClassificationMaskWordCount> &
    words() const noexcept
    {
        return words_;
    }

    bool operator==(const PointClassificationFilter &) const = default;

private:
    std::array<std::uint32_t, pointClassificationMaskWordCount> words_;
};

} // namespace pci
