#include "pointcloud/PointColorMapCatalog.h"
#include "renderer/PointColorMapAtlas.h"
#include "renderer/rhi/EyeDomeLightingPass.h"
#include "renderer/rhi/PointCloudRenderer.h"
#include "support/TestPointColorMaps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

TEST_CASE("block scalar normalization preserves UTM-scale coordinate detail",
          "[qt][renderer]")
{
    constexpr double layerMinimum = 500'000.0;
    constexpr double layerMaximum = 500'010.0;
    constexpr double blockOrigin = layerMinimum;
    const double blockScale =
        (layerMaximum - layerMinimum) / pci::blockQuantizationSteps;

    const pci::ScalarNormalization normalization =
        pci::blockScalarNormalization(
            blockOrigin, blockScale, layerMinimum, layerMaximum);

    const auto normalized = [&normalization](const std::uint16_t value) {
        return normalization.offset +
               static_cast<float>(value) * normalization.step;
    };
    CHECK(normalized(0) == Catch::Approx(0.0F).margin(1e-7F));
    CHECK(normalized(65535) == Catch::Approx(1.0F).margin(1e-6F));
    CHECK(normalized(2) > normalized(1));

    // The former shader path reconstructed both positions as absolute floats.
    // At a UTM easting, adjacent millimetre-scale quantization steps collapse
    // to the same value before normalization.
    const float legacyFirst =
        static_cast<float>(blockOrigin) + static_cast<float>(blockScale);
    const float legacySecond =
        static_cast<float>(blockOrigin) + 2.0F * static_cast<float>(blockScale);
    CHECK(legacyFirst == legacySecond);
}

TEST_CASE("eye-dome response is stable and only darkens occluded depths",
          "[qt][renderer][edl]")
{
    const std::vector<float> flat(8, 10.0F);
    CHECK(pci::eyeDomeLightingShade(10.0F, flat) == Catch::Approx(1.0F));

    std::vector<float> closer = flat;
    closer[0] = 5.0F;
    const float occluded = pci::eyeDomeLightingShade(10.0F, closer);
    CHECK(occluded > 0.0F);
    CHECK(occluded < 1.0F);

    std::vector<float> farther = flat;
    farther[0] = 20.0F;
    CHECK(pci::eyeDomeLightingShade(10.0F, farther) == Catch::Approx(1.0F));

    for (float &depth : closer) {
        depth *= 1000.0F;
    }
    CHECK(pci::eyeDomeLightingShade(10'000.0F, closer) ==
          Catch::Approx(occluded).margin(1e-6F));
}

TEST_CASE("eye-dome response handles invalid input without artifacts",
          "[qt][renderer][edl]")
{
    const std::vector<float> neighbours{
        5.0F,
        0.0F,
        -1.0F,
        std::numeric_limits<float>::infinity(),
    };
    CHECK(pci::eyeDomeLightingShade(0.0F, neighbours) == 1.0F);
    CHECK(pci::eyeDomeLightingShade(std::numeric_limits<float>::quiet_NaN(),
                                    neighbours) == 1.0F);
    CHECK(pci::eyeDomeLightingShade(10.0F, neighbours, 0.0F) == 1.0F);
    CHECK(pci::eyeDomeLightingShade(10.0F, {}) == 1.0F);
}

TEST_CASE("block scalar normalization handles block offsets and bad spans",
          "[qt][renderer]")
{
    const pci::ScalarNormalization normalization =
        pci::blockScalarNormalization(500'040.0, 0.01, 500'000.0, 500'100.0);
    CHECK(normalization.offset == Catch::Approx(0.4F).margin(1e-7F));
    CHECK(normalization.step == Catch::Approx(0.0001F).margin(1e-8F));

    CHECK(pci::blockScalarNormalization(1.0, 1.0, 5.0, 5.0).step == 0.0F);
    CHECK(pci::blockScalarNormalization(1.0, 1.0, 6.0, 5.0).step == 0.0F);
}

TEST_CASE("uniform draw capacity grows geometrically without overflow",
          "[qt][renderer][metrics]")
{
    CHECK(pci::grownUniformDrawCapacity(256, 0) == 256);
    CHECK(pci::grownUniformDrawCapacity(256, 256) == 256);
    CHECK(pci::grownUniformDrawCapacity(256, 257) == 512);
    CHECK(pci::grownUniformDrawCapacity(512, 513) == 1024);

    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    CHECK(pci::grownUniformDrawCapacity(maximum / 2 + 1, maximum) == maximum);
    CHECK(pci::grownUniformDrawCapacity(0, 513) == 513);
}

TEST_CASE("uniform staging packs draws and zeroes alignment gaps",
          "[qt][renderer][batching]")
{
    std::vector<pci::BlockDraw> draws(2);
    draws[0].uniform.pointSize = 2.5F;
    draws[0].uniform.idBase = 17;
    draws[0].uniform.classificationMask[0] = 0x00000004U;
    draws[1].uniform.pointSize = 7.0F;
    draws[1].uniform.idBase = 42;
    draws[1].uniform.classificationMask[7] = 0x80000000U;
    constexpr std::size_t stride = sizeof(pci::BlockUniform) + 32;

    const std::vector<std::byte> staged =
        pci::stageBlockUniforms(draws, stride);
    REQUIRE(staged.size() == 2 * stride);

    pci::BlockUniform first;
    pci::BlockUniform second;
    std::memcpy(&first, staged.data(), sizeof(first));
    std::memcpy(&second, staged.data() + stride, sizeof(second));
    CHECK(first.pointSize == 2.5F);
    CHECK(first.idBase == 17);
    CHECK(first.classificationMask[0] == 0x00000004U);
    CHECK(second.pointSize == 7.0F);
    CHECK(second.idBase == 42);
    CHECK(second.classificationMask[7] == 0x80000000U);
    CHECK(std::ranges::all_of(
        staged.begin() + static_cast<std::ptrdiff_t>(sizeof(first)),
        staged.begin() + static_cast<std::ptrdiff_t>(stride),
        [](const std::byte value) {
            return value == std::byte{};
        }));
    CHECK(std::ranges::all_of(
        staged.begin() + static_cast<std::ptrdiff_t>(stride + sizeof(second)),
        staged.end(),
        [](const std::byte value) {
            return value == std::byte{};
        }));
}

TEST_CASE("uniform staging rejects an undersized stride",
          "[qt][renderer][batching]")
{
    const std::vector<pci::BlockDraw> draws(1);
    CHECK_THROWS_AS(
        pci::stageBlockUniforms(draws, sizeof(pci::BlockUniform) - 1),
        std::invalid_argument);
}

TEST_CASE("color map atlas provides padded portable lookup rows",
          "[qt][renderer][color]")
{
    const auto catalog = pci::test::createTestPointColorMapCatalog();
    const QImage atlas = pci::buildPointColorMapAtlas(*catalog);
    REQUIRE_FALSE(atlas.isNull());
    CHECK(atlas.width() == pci::pointColorMapAtlasWidth);
    CHECK(atlas.height() ==
          static_cast<int>(pci::pointColorMapCatalog(*catalog).size()) *
              pci::pointColorMapAtlasRowsPerMap);

    const auto viridis =
        pci::pointColorMapSampling(*catalog, pci::PointColorMap::Viridis);
    const int row = static_cast<int>(viridis.rowCoordinate *
                                     static_cast<float>(atlas.height()));
    REQUIRE(row > 0);
    REQUIRE(row + 1 < atlas.height());
    CHECK(atlas.pixelColor(0, row - 1) == atlas.pixelColor(0, row));
    CHECK(atlas.pixelColor(0, row + 1) == atlas.pixelColor(0, row));
    CHECK(atlas.pixelColor(0, row) != atlas.pixelColor(atlas.width() - 1, row));
}

} // namespace
