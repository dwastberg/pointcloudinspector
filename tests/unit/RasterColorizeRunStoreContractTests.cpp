#include <pci/operations/RasterColorizeRunStore.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("operation run-store codec preserves its independent byte contract",
          "[unit][operation-api][colorize][golden]")
{
    const pci::RasterSortRecord record{
        .address = 0x0807060504030201ULL,
        .destination = 0x0c0b0a09U,
    };
    const std::array expected{
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},
        std::byte{0x07},
        std::byte{0x08},
        std::byte{0x09},
        std::byte{0x0a},
        std::byte{0x0b},
        std::byte{0x0c},
    };

    const auto encoded = pci::serializeRasterSortRecord(record);
    STATIC_REQUIRE(encoded.size() == 12);
    CHECK(encoded == expected);
    CHECK(pci::deserializeRasterSortRecord(expected) == record);
}
