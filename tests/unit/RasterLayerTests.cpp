#include "raster/RasterLayer.h"
#include "raster/RasterTileSource.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::array<double, 6> northUpTransform{
    1000.0, 2.0, 0.0, 5000.0, 0.0, -2.0};

// A rotated, skewed transform with a nonzero determinant.
constexpr std::array<double, 6> rotatedTransform{
    100.0, 0.5, -0.25, 200.0, 0.25, -0.5};

[[nodiscard]] pci::RasterLevel makeLevel(const std::uint32_t width,
                                         const std::uint32_t height,
                                         const std::uint32_t baseWidth,
                                         const std::uint32_t baseHeight)
{
    pci::RasterLevel level;
    level.width = width;
    level.height = height;
    level.basePixelsPerTexelX =
        static_cast<double>(baseWidth) / static_cast<double>(width);
    level.basePixelsPerTexelY =
        static_cast<double>(baseHeight) / static_cast<double>(height);
    level.channelCount = 3;
    return level;
}

TEST_CASE("raster affine rejects singular transforms", "[unit][raster]")
{
    CHECK(pci::rasterAffineInvertible(northUpTransform));
    CHECK(pci::rasterAffineInvertible(rotatedTransform));

    CHECK_FALSE(pci::rasterAffineInvertible({0.0, 1.0, 2.0, 0.0, 2.0, 4.0}));
    CHECK_FALSE(pci::rasterAffineInvertible({0.0, 0.0, 0.0, 0.0, 0.0, 0.0}));

    const double notANumber = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(
        pci::rasterAffineInvertible({notANumber, 2.0, 0.0, 0.0, 0.0, -2.0}));
    CHECK_FALSE(pci::rasterAffineInvertible(
        {0.0, std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0, -2.0}));
}

TEST_CASE("raster affine tolerance is scale relative", "[unit][raster]")
{
    // A degree-scale transform has a tiny determinant but is perfectly usable;
    // comparing against machine epsilon directly would reject it.
    constexpr std::array<double, 6> degreeScale{
        11.0, 1.0e-5, 0.0, 59.0, 0.0, -1.0e-5};
    CHECK(pci::rasterAffineInvertible(degreeScale));

    // Nearly singular relative to its own scale.
    constexpr std::array<double, 6> nearlySingular{
        0.0, 1.0, 1.0, 0.0, 1.0, 1.0 + 1.0e-15};
    CHECK_FALSE(pci::rasterAffineInvertible(nearlySingular));
}

TEST_CASE("raster affine round trips through the inverse", "[unit][raster]")
{
    const pci::Vec3d world =
        pci::rasterPixelToWorld(rotatedTransform, 3.0, 7.0);
    const auto pixel =
        pci::rasterWorldToPixel(rotatedTransform, world.x, world.y);
    REQUIRE(pixel.has_value());
    CHECK(pixel->pixel == Catch::Approx(3.0));
    CHECK(pixel->line == Catch::Approx(7.0));

    CHECK_FALSE(
        pci::rasterWorldToPixel({0.0, 1.0, 2.0, 0.0, 2.0, 4.0}, 1.0, 1.0)
            .has_value());
}

TEST_CASE("raster geometry uses pixel edges, not centers", "[unit][raster]")
{
    const std::array<pci::Vec3d, 4> corners =
        pci::rasterCornerPoints(northUpTransform, 10, 20);

    CHECK(corners[0] == pci::Vec3d{1000.0, 5000.0, 0.0});
    CHECK(corners[1] == pci::Vec3d{1020.0, 5000.0, 0.0});
    CHECK(corners[2] == pci::Vec3d{1020.0, 4960.0, 0.0});
    CHECK(corners[3] == pci::Vec3d{1000.0, 4960.0, 0.0});
}

TEST_CASE("raster bounds cover all four rotated corners", "[unit][raster]")
{
    const auto northUp = pci::rasterPixelEdgeBounds(northUpTransform, 10, 20);
    REQUIRE(northUp.has_value());
    CHECK(northUp->minimum[0] == Catch::Approx(1000.0));
    CHECK(northUp->minimum[1] == Catch::Approx(4960.0));
    CHECK(northUp->maximum[0] == Catch::Approx(1020.0));
    CHECK(northUp->maximum[1] == Catch::Approx(5000.0));

    // A rotated footprint's bounds must come from every corner; taking only
    // the first and last would understate the extent.
    const auto rotated = pci::rasterPixelEdgeBounds(rotatedTransform, 8, 4);
    REQUIRE(rotated.has_value());
    const std::array<pci::Vec3d, 4> corners =
        pci::rasterCornerPoints(rotatedTransform, 8, 4);
    for (const pci::Vec3d corner : corners) {
        CHECK(corner.x >= rotated->minimum[0]);
        CHECK(corner.x <= rotated->maximum[0]);
        CHECK(corner.y >= rotated->minimum[1]);
        CHECK(corner.y <= rotated->maximum[1]);
    }

    CHECK_FALSE(
        pci::rasterPixelEdgeBounds(northUpTransform, 0, 20).has_value());
    CHECK_FALSE(pci::rasterPixelEdgeBounds({0.0, 1.0, 2.0, 0.0, 2.0, 4.0}, 4, 4)
                    .has_value());
}

TEST_CASE("raster antimeridian detection needs a geographic CRS",
          "[unit][raster]")
{
    pci::Bounds3d wrapped;
    wrapped.minimum = {-179.5, -10.0, 0.0};
    wrapped.maximum = {179.5, 10.0, 0.0};
    CHECK(pci::rasterCrossesAntimeridian(wrapped, true));
    CHECK_FALSE(pci::rasterCrossesAntimeridian(wrapped, false));

    pci::Bounds3d narrow;
    narrow.minimum = {10.0, -10.0, 0.0};
    narrow.maximum = {20.0, 10.0, 0.0};
    CHECK_FALSE(pci::rasterCrossesAntimeridian(narrow, true));
}

TEST_CASE("raster level table rejects malformed pyramids", "[unit][raster]")
{
    std::vector<pci::RasterLevel> levels{
        makeLevel(1000, 800, 1000, 800),
        makeLevel(500, 400, 1000, 800),
        makeLevel(250, 200, 1000, 800),
    };
    CHECK(pci::rasterLevelTableValid(levels, 1000, 800));

    SECTION("entry zero must be the base dimensions")
    {
        CHECK_FALSE(pci::rasterLevelTableValid(levels, 999, 800));
    }

    SECTION("duplicate entries are rejected")
    {
        levels[2] = levels[1];
        CHECK_FALSE(pci::rasterLevelTableValid(levels, 1000, 800));
    }

    SECTION("non-decreasing entries are rejected")
    {
        levels[2] = makeLevel(600, 480, 1000, 800);
        CHECK_FALSE(pci::rasterLevelTableValid(levels, 1000, 800));
    }

    SECTION("a level with no selected channels is rejected")
    {
        levels[1].channelCount = 0;
        CHECK_FALSE(pci::rasterLevelTableValid(levels, 1000, 800));
    }

    SECTION("asymmetric reduction in one dimension is accepted")
    {
        const std::vector<pci::RasterLevel> asymmetric{
            makeLevel(1000, 800, 1000, 800),
            makeLevel(500, 800, 1000, 800),
        };
        CHECK(pci::rasterLevelTableValid(asymmetric, 1000, 800));
    }

    CHECK_FALSE(pci::rasterLevelTableValid({}, 1000, 800));
}

TEST_CASE("raster coverage pyramid ends in a single logical tile",
          "[unit][raster][overview]")
{
    std::vector<pci::RasterLevel> levels{makeLevel(10000, 10000, 10000, 10000)};

    pci::appendGeneratedRasterCoverageLevels(levels, 10000, 10000);

    REQUIRE(levels.size() == 7);
    CHECK(pci::rasterBackedLevelCount(levels) == 1);
    CHECK(pci::rasterGeneratedLevelCount(levels) == 6);
    CHECK(levels[1].width == 5000);
    CHECK(levels[5].width == 313);
    CHECK(levels.back().width == 157);
    CHECK(levels.back().height == 157);
    CHECK(levels.back().kind == pci::RasterLevelKind::GeneratedCoverage);
    CHECK(levels.back().basePixelsPerTexelX == Catch::Approx(10000.0 / 157.0));
    CHECK(pci::rasterLevelTableValid(levels, 10000, 10000));

    // Logical preview levels do not change whether the source itself has an
    // adequate backed overview, and constructing them is idempotent.
    pci::RasterLayerMetadata metadata;
    metadata.width = 10000;
    metadata.height = 10000;
    metadata.levels = levels;
    CHECK(pci::rasterRequiresTiledRendering(metadata));
    pci::appendGeneratedRasterCoverageLevels(levels, 10000, 10000);
    CHECK(levels.size() == 7);
}

TEST_CASE("raster tile counts and edge extents", "[unit][raster]")
{
    const pci::RasterLevel level = makeLevel(1000, 800, 1000, 800);
    CHECK(pci::rasterLevelTileCountX(level) == 4);
    CHECK(pci::rasterLevelTileCountY(level) == 4);

    CHECK(pci::rasterTileValidExtent(level, {0, 0, 0}) ==
          pci::RasterTileExtent{256, 256});
    // The last row and column are short: 1000 - 768 and 800 - 768.
    CHECK(pci::rasterTileValidExtent(level, {0, 3, 3}) ==
          pci::RasterTileExtent{232, 32});
    CHECK(pci::rasterTileValidExtent(level, {0, 4, 0}) ==
          pci::RasterTileExtent{0, 0});
    CHECK(pci::rasterTileValidExtent(level, {0, 0, 4}) ==
          pci::RasterTileExtent{0, 0});
}

TEST_CASE("raster tile base-pixel rect snaps the outer edge", "[unit][raster]")
{
    // A non-power-of-two reduction whose ratio does not multiply back to the
    // base dimensions exactly.
    const pci::RasterLevel level = makeLevel(333, 266, 1000, 800);
    REQUIRE(pci::rasterLevelTileCountX(level) == 2);
    REQUIRE(pci::rasterLevelTileCountY(level) == 2);

    const pci::RasterBasePixelRect first =
        pci::rasterTileBasePixelRect(level, {1, 0, 0}, 1000, 800);
    CHECK(first.minimumPixel == 0.0);
    CHECK(first.minimumLine == 0.0);

    const pci::RasterBasePixelRect last =
        pci::rasterTileBasePixelRect(level, {1, 1, 1}, 1000, 800);
    CHECK(last.maximumPixel == 1000.0);
    CHECK(last.maximumLine == 800.0);

    // Adjacent tiles share an identical edge in base-pixel coordinates.
    CHECK(first.maximumPixel == last.minimumPixel);
    CHECK(first.maximumLine == last.minimumLine);

    CHECK(pci::rasterTileBasePixelRect(level, {1, 9, 9}, 1000, 800) ==
          pci::RasterBasePixelRect{});
}

TEST_CASE("raster base-pixel overlap relates arbitrary levels",
          "[unit][raster]")
{
    const pci::RasterLevel fine = makeLevel(1000, 800, 1000, 800);
    const pci::RasterLevel coarse = makeLevel(333, 266, 1000, 800);

    const pci::RasterBasePixelRect parent =
        pci::rasterTileBasePixelRect(coarse, {1, 0, 0}, 1000, 800);
    const pci::RasterBasePixelRect sibling =
        pci::rasterTileBasePixelRect(coarse, {1, 1, 1}, 1000, 800);
    const pci::RasterBasePixelRect child =
        pci::rasterTileBasePixelRect(fine, {0, 0, 0}, 1000, 800);

    CHECK(parent.overlaps(child));
    CHECK(child.overlaps(parent));
    CHECK_FALSE(sibling.overlaps(child));

    // Adjacent coarse tiles share an edge exactly; a half-open comparison must
    // not report that shared edge as an overlap.
    CHECK(parent.maximumPixel == sibling.minimumPixel);
    CHECK_FALSE(parent.overlaps(sibling));

    // The ancestor relation is not x / 2: this parent reaches past the fine
    // level's third tile boundary because its reduction ratio is 3.003.
    const pci::RasterBasePixelRect straddled =
        pci::rasterTileBasePixelRect(fine, {0, 3, 3}, 1000, 800);
    CHECK(parent.overlaps(straddled));
}

TEST_CASE("raster style clamps invalid values", "[unit][raster]")
{
    pci::RasterLayerStyle style;
    style.opacity = std::numeric_limits<float>::infinity();
    style.zOffset = std::numeric_limits<double>::quiet_NaN();
    const pci::RasterLayerStyle clamped = pci::clampRasterLayerStyle(style);
    CHECK(clamped.opacity == 1.0F);
    CHECK(clamped.zOffset == 0.0);
    CHECK(clamped.verticalExaggeration == 1.0);
    CHECK(clamped.surfaceShadingStrength == 1.0F);

    style.opacity = -1.0F;
    style.zOffset = 1.0e9;
    const pci::RasterLayerStyle bounded = pci::clampRasterLayerStyle(style);
    CHECK(bounded.opacity == 0.0F);
    CHECK(bounded.zOffset == pci::maximumRasterZOffsetMagnitude);

    style.verticalExaggeration = 1000.0;
    style.surfaceShadingStrength = -2.0F;
    const pci::RasterLayerStyle terrain =
        pci::clampRasterLayerStyle(style);
    CHECK(terrain.verticalExaggeration ==
          pci::maximumRasterVerticalExaggeration);
    CHECK(terrain.surfaceShadingStrength == 0.0F);

    pci::RasterLayerStyle ranged;
    ranged.displayRange =
        pci::RasterDisplayRange{.minimum = 90.0, .maximum = 10.0};
    const pci::RasterLayerStyle swapped = pci::clampRasterLayerStyle(ranged);
    REQUIRE(swapped.displayRange.has_value());
    CHECK(swapped.displayRange->minimum == 10.0);
    CHECK(swapped.displayRange->maximum == 90.0);

    ranged.displayRange = pci::RasterDisplayRange{
        .minimum = std::numeric_limits<double>::quiet_NaN(), .maximum = 1.0};
    CHECK_FALSE(pci::clampRasterLayerStyle(ranged).displayRange.has_value());
}

TEST_CASE("raster default style colorizes scalar sources", "[unit][raster]")
{
    pci::RasterLayerMetadata metadata;
    metadata.defaultDisplay.sampleKind = pci::RasterSampleKind::ContinuousColor;
    CHECK(pci::defaultRasterLayerStyle(metadata).colorRampKey.empty());

    metadata.defaultDisplay.sampleKind =
        pci::RasterSampleKind::ContinuousScalar;
    metadata.defaultDisplay.displayRange =
        pci::RasterDisplayRange{.minimum = -5.0, .maximum = 120.0};
    const pci::RasterLayerStyle style = pci::defaultRasterLayerStyle(metadata);
    CHECK(style.colorRampKey == pci::defaultRasterScalarColorRampKey);
    REQUIRE(style.displayRange.has_value());
    CHECK(style.displayRange->minimum == -5.0);
    CHECK(style.opacity == 1.0F);
    CHECK(style.zOffset == 0.0);
    CHECK(style.renderMode == pci::RasterRenderMode::Flat);
    CHECK(style.verticalExaggeration == 1.0);
}

TEST_CASE("surface bounds require a ready exact elevation range",
          "[unit][raster][surface]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 10;
    metadata.height = 20;
    metadata.geoTransform = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.elevation.available = true;

    pci::RasterLayerStyle style;
    style.renderMode = pci::RasterRenderMode::Surface;
    style.verticalExaggeration = 2.0;
    style.zOffset = 5.0;
    const pci::RasterElevationRange range{.minimum = -10.0,
                                          .maximum = 30.0};

    CHECK(pci::rasterEffectiveRenderMode(metadata,
                                         style,
                                         pci::RasterElevationStatus::Scanning,
                                         range) ==
          pci::RasterRenderMode::Flat);
    const pci::Bounds3d pending = pci::rasterSceneBounds(
        metadata, style, pci::RasterElevationStatus::Scanning, range);
    CHECK(pending.minimum[2] == Catch::Approx(4.5));
    CHECK(pending.maximum[2] == Catch::Approx(5.5));

    CHECK(pci::rasterEffectiveRenderMode(metadata,
                                         style,
                                         pci::RasterElevationStatus::Ready,
                                         range) ==
          pci::RasterRenderMode::Surface);
    const pci::Bounds3d ready = pci::rasterSceneBounds(
        metadata, style, pci::RasterElevationStatus::Ready, range);
    CHECK(ready.minimum[2] == Catch::Approx(-15.0));
    CHECK(ready.maximum[2] == Catch::Approx(65.0));

    const pci::Bounds3d constant = pci::rasterSceneBounds(
        metadata,
        style,
        pci::RasterElevationStatus::Ready,
        pci::RasterElevationRange{.minimum = 7.0, .maximum = 7.0});
    CHECK(constant.maximum[2] - constant.minimum[2] == Catch::Approx(1.0));
}

TEST_CASE("raster tile keys hash and order", "[unit][raster]")
{
    const pci::RasterTileKey key{2, 5, 9};
    CHECK(std::hash<pci::RasterTileKey>{}(key) ==
          std::hash<pci::RasterTileKey>{}(pci::RasterTileKey{2, 5, 9}));

    std::unordered_set<pci::RasterTileKey> keys;
    keys.insert(key);
    keys.insert({2, 5, 9});
    keys.insert({2, 5, 10});
    CHECK(keys.size() == 2);

    // Ordering by level then row then column makes the prior-selection lookup
    // a bounded range scan.
    CHECK(pci::RasterTileKey{1, 0, 0} < pci::RasterTileKey{2, 0, 0});
    CHECK(pci::RasterTileKey{2, 0, 1} < pci::RasterTileKey{2, 1, 0});
}

TEST_CASE("raster tile data accounts its allocation", "[unit][raster]")
{
    CHECK(pci::rasterStoredTileBytes == 258U * 258U * 4U);

    pci::RasterTileData tile;
    tile.rgba.resize(pci::rasterStoredTileBytes);
    tile.validWidth = 256;
    tile.validHeight = 256;
    CHECK(tile.byteSize() >= pci::rasterStoredTileBytes);
    CHECK(tile.byteSize() > tile.rgba.size());
    const std::uint64_t colorBytes = tile.byteSize();
    tile.profile = pci::RasterTilePayloadProfile::RenderElevation;
    tile.elevation.resize(pci::rasterStoredTilePixels *
                          pci::rasterStoredTilePixels);
    CHECK(tile.byteSize() >= colorBytes +
                                 tile.elevation.size() * sizeof(float));
}

TEST_CASE("raster source identifiers are monotonic", "[unit][raster]")
{
    const pci::RasterSourceId first = pci::nextRasterSourceId();
    const pci::RasterSourceId second = pci::nextRasterSourceId();
    CHECK(first.value() < second.value());
    CHECK(first != second);
}

} // namespace

TEST_CASE("raster overview coverage decides the missing-overview warning",
          "[unit][raster]")
{
    // The most common thing a user drags in first: an un-overviewed mid-size
    // GeoTIFF whose only level is its base. It renders through the tiled path
    // at native resolution, so it earns no warning.
    pci::RasterLayerMetadata midSize;
    midSize.width = 5000;
    midSize.height = 5000;
    midSize.levels = {makeLevel(5000, 5000, 5000, 5000)};
    CHECK_FALSE(pci::rasterRequiresTiledRendering(midSize));

    // Far above the threshold, covering the view from the base band alone
    // would take thousands of native-resolution tiles. That is a missing
    // overview, and the layer says so rather than silently showing a fraction
    // of the image.
    pci::RasterLayerMetadata huge;
    huge.width = 100000;
    huge.height = 100000;
    huge.levels = {makeLevel(100000, 100000, 100000, 100000)};
    CHECK(pci::rasterRequiresTiledRendering(huge));

    // The warning clears the moment a level coarse enough to cover the raster
    // exists, without any change to the threshold.
    huge.levels.push_back(makeLevel(3125, 3125, 100000, 100000));
    CHECK_FALSE(pci::rasterRequiresTiledRendering(huge));

    // A source with no level table at all can display nothing.
    CHECK(pci::rasterRequiresTiledRendering({}));
}
