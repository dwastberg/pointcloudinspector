#include <pci/operations/RasterColorizeRunStore.h>

namespace pci {

std::array<std::byte, 12>
serializeRasterSortRecord(const RasterSortRecord &record) noexcept
{
    std::array<std::byte, 12> result{};
    for (std::uint32_t index = 0; index < 8; ++index) {
        result[index] = static_cast<std::byte>(record.address >> (index * 8U));
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        result[8U + index] =
            static_cast<std::byte>(record.destination >> (index * 8U));
    }
    return result;
}

RasterSortRecord
deserializeRasterSortRecord(const std::span<const std::byte, 12> bytes) noexcept
{
    RasterSortRecord result;
    for (std::uint32_t index = 0; index < 8; ++index) {
        result.address |= static_cast<std::uint64_t>(
                              std::to_integer<std::uint8_t>(bytes[index]))
                          << (index * 8U);
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        result.destination |=
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[8U + index]))
            << (index * 8U);
    }
    return result;
}

} // namespace pci
