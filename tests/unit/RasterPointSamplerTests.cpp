#include "raster/RasterPointSampler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <random>

namespace {

[[nodiscard]] pci::RasterLayerMetadata metadata(
    const std::uint32_t width = 300,
    const std::uint32_t height = 300,
    const std::array<double, 6> transform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0})
{
    return {.width = width, .height = height, .geoTransform = transform};
}

[[nodiscard]] std::uint32_t storedOffset(const std::uint32_t innerX,
                                         const std::uint32_t innerY)
{
    return ((innerY + pci::rasterTileGutter) * pci::rasterStoredTilePixels +
            innerX + pci::rasterTileGutter) *
           4U;
}

TEST_CASE("raster point placement uses pixel edges and floor",
          "[unit][raster][colorize]")
{
    const auto placement =
        pci::RasterInversePlacement::forMetadata(metadata(4, 4));
    REQUIRE(placement.has_value());

    CHECK(placement->addressOf(0.0, 0.0) ==
          pci::RasterPixelAddress{.tile = {0, 0, 0}, .innerX = 0, .innerY = 0});
    CHECK(placement->addressOf(0.999, -0.001) ==
          pci::RasterPixelAddress{.tile = {0, 0, 0}, .innerX = 0, .innerY = 0});
    CHECK(placement->addressOf(1.0, -1.0) ==
          pci::RasterPixelAddress{.tile = {0, 0, 0}, .innerX = 1, .innerY = 1});

    INFO("Nearest-integer placement would be wrong at fractional pixel 0.6");
    CHECK(std::lround(0.6) == 1);
    REQUIRE(placement->addressOf(0.6, -0.6).has_value());
    CHECK(placement->addressOf(0.6, -0.6)->innerX == 0);
    CHECK(placement->addressOf(0.6, -0.6)->innerY == 0);

    CHECK_FALSE(placement->addressOf(-0.001, 0.0));
    CHECK_FALSE(placement->addressOf(4.0, -1.0));
    CHECK_FALSE(placement->addressOf(1.0, 0.001));
    CHECK_FALSE(placement->addressOf(1.0, -4.0));
    CHECK_FALSE(
        placement->addressOf(std::numeric_limits<double>::quiet_NaN(), 0.0));
    CHECK_FALSE(
        placement->addressOf(std::numeric_limits<double>::infinity(), 0.0));
}

TEST_CASE("raster point addresses split tiles without including gutters",
          "[unit][raster][colorize]")
{
    const auto placement = pci::RasterInversePlacement::forMetadata(metadata());
    REQUIRE(placement.has_value());
    CHECK(placement->addressOf(256.0, 0.0) ==
          pci::RasterPixelAddress{.tile = {0, 1, 0}, .innerX = 0, .innerY = 0});
    CHECK(storedOffset(0, 0) == (pci::rasterStoredTilePixels + 1U) * 4U);
    CHECK(placement->addressOf(255.0, -255.0) ==
          pci::RasterPixelAddress{
              .tile = {0, 0, 0}, .innerX = 255, .innerY = 255});
    CHECK(storedOffset(255, 255) ==
          (256U * pci::rasterStoredTilePixels + 256U) * 4U);

    const auto last = placement->addressOf(299.999, -299.999);
    REQUIRE(last.has_value());
    CHECK(last->tile == pci::RasterTileKey{0, 1, 1});
    CHECK(last->innerX == 43);
    CHECK(last->innerY == 43);
    CHECK_FALSE(placement->addressOf(300.0, -299.0));
    CHECK_FALSE(placement->addressOf(299.0, -300.0));
}

TEST_CASE("block raster placement agrees with generic affine inversion",
          "[unit][raster][colorize]")
{
    constexpr std::array<double, 6> transforms[]{
        {10.0, 0.25, 0.0, 20.0, 0.0, -0.25},
        {100.0, 0.5, -0.25, 200.0, 0.25, -0.5},
    };
    for (const auto &transform : transforms) {
        const auto placement = pci::RasterInversePlacement::forMetadata(
            metadata(200'000, 200'000, transform));
        REQUIRE(placement.has_value());
        constexpr pci::Vec3d origin{105.0, 180.0, 0.0};
        constexpr double scale = 0.00025;
        const auto block = placement->forBlock(origin.x, origin.y, scale);
        for (const std::uint16_t value :
             {std::uint16_t{0},
              std::uint16_t{32768},
              std::numeric_limits<std::uint16_t>::max()}) {
            CAPTURE(transform, value);
            CHECK(placement->addressOf(block, value, value) ==
                  placement->addressOf(origin.x + scale * value,
                                       origin.y + scale * value));
        }
    }
}

TEST_CASE("rotated raster point placement round trips pixel centers",
          "[unit][raster][colorize]")
{
    constexpr std::array<double, 6> transform{
        100.0, 0.5, -0.25, 200.0, 0.25, -0.5};
    const auto placement =
        pci::RasterInversePlacement::forMetadata(metadata(16, 16, transform));
    REQUIRE(placement.has_value());
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const pci::Vec3d world =
                pci::rasterPixelToWorld(transform, x + 0.5, y + 0.5);
            const auto address = placement->addressOf(world.x, world.y);
            REQUIRE(address.has_value());
            CHECK(address->innerX == x);
            CHECK(address->innerY == y);
        }
    }

    // Fixed seed makes the 10,000 affine comparisons reproducible.
    std::mt19937_64 random(20260815); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<double> coordinate(0.0, 15.999999);
    for (std::size_t sample = 0; sample < 10'000; ++sample) {
        const double x = coordinate(random);
        const double y = coordinate(random);
        const pci::Vec3d world = pci::rasterPixelToWorld(transform, x, y);
        const auto reference =
            pci::rasterWorldToPixel(transform, world.x, world.y);
        const auto address = placement->addressOf(world.x, world.y);
        REQUIRE(reference.has_value());
        REQUIRE(address.has_value());
        CHECK(address->innerX ==
              static_cast<std::uint32_t>(std::floor(reference->pixel)));
        CHECK(address->innerY ==
              static_cast<std::uint32_t>(std::floor(reference->line)));
    }
}

TEST_CASE("raster texels unpremultiply and pack opaque point colors",
          "[unit][raster][colorize]")
{
    pci::RasterTileData tile;
    tile.rgba.resize(pci::rasterStoredTileBytes);
    constexpr std::uint32_t offset = 40;

    tile.rgba[offset + 0] = std::byte{12};
    tile.rgba[offset + 1] = std::byte{34};
    tile.rgba[offset + 2] = std::byte{56};
    tile.rgba[offset + 3] = std::byte{255};
    CHECK(pci::rasterTexelToPointColor(tile, offset) == 0xff38220cU);

    tile.rgba[offset + 0] = std::byte{64};
    tile.rgba[offset + 1] = std::byte{32};
    tile.rgba[offset + 2] = std::byte{200}; // Deliberately greater than alpha.
    tile.rgba[offset + 3] = std::byte{128};
    CHECK(pci::rasterTexelToPointColor(tile, offset) == 0xffff4080U);

    tile.rgba[offset + 0] = std::byte{1};
    tile.rgba[offset + 1] = std::byte{0};
    tile.rgba[offset + 2] = std::byte{0};
    tile.rgba[offset + 3] = std::byte{2};
    CHECK(pci::rasterTexelToPointColor(tile, offset) == 0xff000080U);

    tile.rgba[offset + 3] = std::byte{0};
    CHECK_FALSE(pci::rasterTexelToPointColor(tile, offset));
    CHECK_FALSE(pci::rasterTexelToPointColor(
        tile, static_cast<std::uint32_t>(tile.rgba.size() - 3)));
}

TEST_CASE("degenerate raster placement is rejected", "[unit][raster][colorize]")
{
    CHECK_FALSE(pci::RasterInversePlacement::forMetadata(
        metadata(4, 4, {0.0, 1.0, 2.0, 0.0, 2.0, 4.0})));
    CHECK_FALSE(pci::RasterInversePlacement::forMetadata(metadata(0, 4)));
}

} // namespace
