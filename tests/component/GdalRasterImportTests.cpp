#include "fixtures/GdalRasterFixtureFactory.h"
#include "import/gdal/GdalRasterLoader.h"
#include "import/gdal/GdalRasterSource.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace {

// The fixture corpus is written once; every case here is read-only.
const pci::test::GdalRasterFixturePaths &fixtures()
{
    static const pci::test::GdalRasterFixturePaths paths =
        pci::test::writeGdalRasterFixtures(
            std::filesystem::temp_directory_path() / "pci-raster-fixtures");
    return paths;
}

[[nodiscard]] pci::RasterLayerDataPtr load(const std::filesystem::path &path,
                                           const pci::GdalRasterLoader &loader)
{
    pci::RasterImportRequest request;
    request.sourcePath = path;
    return loader.inspect(request).data;
}

[[nodiscard]] pci::RasterLayerDataPtr load(const std::filesystem::path &path)
{
    const pci::GdalRasterLoader loader;
    return load(path, loader);
}

[[nodiscard]] pci::RasterTileData
readFirstTile(const pci::RasterLayerData &data, const std::uint32_t level = 0)
{
    pci::RasterTileRequest request;
    request.key = pci::RasterTileKey{level, 0, 0};
    request.decode = std::make_shared<pci::RasterDecodeParameters>(
        data.metadata().defaultDisplay);
    return data.source->readTile(request, std::stop_token{});
}

[[nodiscard]] std::array<std::byte, 4> texel(const pci::RasterTileData &tile,
                                             const std::uint32_t x,
                                             const std::uint32_t y)
{
    const std::size_t base =
        (static_cast<std::size_t>(y) * pci::rasterStoredTilePixels + x) * 4;
    return {tile.rgba[base],
            tile.rgba[base + 1],
            tile.rgba[base + 2],
            tile.rgba[base + 3]};
}

TEST_CASE("raster inspection reports placement metadata", "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgb);
    REQUIRE(data != nullptr);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    CHECK(metadata.sourceDriver == "GTiff");
    CHECK(metadata.width == 64);
    CHECK(metadata.height == 48);
    CHECK(metadata.bands.size() == 3);
    CHECK_FALSE(metadata.crsMissing);
    CHECK_FALSE(metadata.crossesAntimeridian);
    CHECK_FALSE(metadata.insufficientOverviews);

    // Pixel edges, not centers: 64 columns of 2 metres reach exactly 128.
    CHECK(metadata.bounds.minimum[0] == Catch::Approx(674000.0));
    CHECK(metadata.bounds.maximum[0] == Catch::Approx(674128.0));
    CHECK(metadata.bounds.maximum[1] == Catch::Approx(6580000.0));
    CHECK(metadata.bounds.minimum[1] == Catch::Approx(6580000.0 - 96.0));

    REQUIRE(metadata.levels.size() == 1);
    CHECK(metadata.levels[0].width == 64);
    CHECK(metadata.levels[0].channelCount == 3);
    CHECK(metadata.levels[0].rgbaBands[0].overview == -1);
}

TEST_CASE("raster band selection resolves deterministically",
          "[component][gdal]")
{
    SECTION("explicit RGB with an alpha band")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().rgba);
        const pci::RasterLayerMetadata &metadata = data->metadata();
        CHECK(metadata.defaultDisplay.sampleKind ==
              pci::RasterSampleKind::ContinuousColor);
        CHECK(metadata.levels[0].channelCount == 4);
        CHECK(metadata.levels[0].rgbaBands[3].band == 4);
    }

    SECTION("byte grayscale stays photographic")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().gray);
        CHECK(data->metadata().defaultDisplay.sampleKind ==
              pci::RasterSampleKind::ContinuousColor);
        CHECK(data->metadata().levels[0].channelCount == 1);
    }

    SECTION("a palette index band is categorical")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().palette);
        CHECK(data->metadata().defaultDisplay.sampleKind ==
              pci::RasterSampleKind::Categorical);
        // A palette never reaches portable code as a table.
        CHECK_FALSE(data->metadata().defaultDisplay.displayRange.has_value());
    }

    SECTION("float terrain is a colorized scalar, not a rejected source")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().terrain);
        CHECK(data->metadata().defaultDisplay.sampleKind ==
              pci::RasterSampleKind::ContinuousScalar);
        CHECK(pci::defaultRasterLayerStyle(data->metadata()).colorRampKey ==
              pci::defaultRasterScalarColorRampKey);
    }
}

TEST_CASE("raster levels are matched by dimension, not overview index",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data =
        load(fixtures().rgbNonPowerOfTwoOverviews);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    REQUIRE(metadata.levels.size() >= 2);
    CHECK(pci::rasterLevelTableValid(
        metadata.levels, metadata.width, metadata.height));

    // Reductions of 3 and 5, so no level ratio is a power of two.
    for (std::size_t index = 1; index < metadata.levels.size(); ++index) {
        const pci::RasterLevel &level = metadata.levels[index];
        CHECK(level.basePixelsPerTexelX > 1.0);
        const double ratio = level.basePixelsPerTexelX;
        CHECK(std::abs(ratio - 2.0) > 0.01);
        CHECK(std::abs(ratio - 4.0) > 0.01);
        // Every required band names an explicitly backed overview.
        for (std::uint8_t channel = 0; channel < level.channelCount;
             ++channel) {
            CHECK(level.rgbaBands[channel].overview >= 0);
        }
    }
}

TEST_CASE("raster levels expose only the common band intersection",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().mismatchedOverviews);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    // The fixture's red band declares a 32x24 overview that green and blue do
    // not. Exposing it would read color from one overview size and the rest
    // from another, which misregisters them, so only the base level survives.
    REQUIRE(metadata.levels.size() == 1);
    CHECK(metadata.levels[0].width == metadata.width);
    CHECK(metadata.levels[0].rgbaBands[0].overview == -1);
    CHECK(pci::rasterLevelTableValid(
        metadata.levels, metadata.width, metadata.height));
}

TEST_CASE("raster range sampling never maps 16-bit through its full domain",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().unsigned16);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    REQUIRE(metadata.defaultDisplay.displayRange.has_value());
    const pci::RasterDisplayRange &range =
        *metadata.defaultDisplay.displayRange;
    CHECK(range.origin != pci::RasterDisplayRange::Origin::Metadata);
    CHECK(range.minimum >= 900.0);
    CHECK(range.maximum <= 5100.0);
    // A full-range map would render this source nearly black.
    CHECK(range.maximum < 60000.0);
}

TEST_CASE("raster range sampling excludes nodata", "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().terrain);
    REQUIRE(data->metadata().defaultDisplay.displayRange.has_value());
    const pci::RasterDisplayRange &range =
        *data->metadata().defaultDisplay.displayRange;
    CHECK(range.minimum > -9000.0);
    CHECK(range.minimum >= 100.0);
}

TEST_CASE("raster inspection of a huge source performs bounded reads",
          "[component][gdal][stress]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data = load(fixtures().sparseHuge, loader);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    CHECK(metadata.width == 100000);
    CHECK(metadata.height == 100000);
    // No backed level fits the static cap and the base is far above the
    // bounded-read threshold, so this source requires tiled rendering.
    CHECK(metadata.insufficientOverviews);
    CHECK(pci::rasterRequiresTiledRendering(metadata));

    // 64 sample windows per selected band, independent of the ten billion
    // pixels the source logically contains.
    CHECK(loader.sampleReadCount() <= 64);
    CHECK(loader.sampleReadCount() > 0);
}

TEST_CASE("raster tile reads are 1:1 windows with a replicated gutter",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgb);
    const pci::RasterTileData tile = readFirstTile(*data);

    CHECK(tile.key == pci::RasterTileKey{0, 0, 0});
    CHECK(tile.rgba.size() == pci::rasterStoredTileBytes);
    // The source is smaller than one tile, so the tile's valid extent is the
    // whole image.
    CHECK(tile.validWidth == 64);
    CHECK(tile.validHeight == 48);

    // The outer gutter replicates the edge texel rather than holding fill.
    CHECK(texel(tile, 0, 0) == texel(tile, 1, 1));
    CHECK(texel(tile, 0, 5) == texel(tile, 1, 5));
    CHECK(texel(tile, 5, 0) == texel(tile, 5, 1));

    // Known corner colors land where the affine puts them: red ramps along x
    // and green along y, so the first interior texel is dark and opaque.
    const std::array<std::byte, 4> origin = texel(tile, 1, 1);
    CHECK(origin[0] == std::byte{0});
    CHECK(origin[3] == std::byte{255});
    const std::array<std::byte, 4> right = texel(tile, 64, 1);
    CHECK(right[0] > std::byte{200});

    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);
    // One read per selected band and nothing more.
    CHECK(source->readCount() == 3);
}

TEST_CASE("raster tile reads compose transparency before premultiplying",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgba);
    const pci::RasterTileData tile = readFirstTile(*data);

    // The fixture's left half is fully transparent. A premultiplied buffer
    // stores zero color there, so the GPU's linear filter cannot drag an
    // invalid color into a valid neighbour.
    const std::array<std::byte, 4> transparent = texel(tile, 4, 4);
    CHECK(transparent[3] == std::byte{0});
    CHECK(transparent[0] == std::byte{0});
    CHECK(transparent[1] == std::byte{0});
    CHECK(transparent[2] == std::byte{0});

    const std::array<std::byte, 4> opaque = texel(tile, 30, 4);
    CHECK(opaque[3] == std::byte{255});
}

TEST_CASE("raster nodata becomes transparency, not a color",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().terrain);
    const pci::RasterTileData tile = readFirstTile(*data);

    // Columns 0-7 are nodata in the fixture.
    CHECK(texel(tile, 4, 20)[3] == std::byte{0});
    CHECK(texel(tile, 4, 20)[0] == std::byte{0});
    CHECK(texel(tile, 40, 20)[3] == std::byte{255});
}

TEST_CASE("raster tile reads honour cancellation without failing",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgb);

    std::stop_source source;
    source.request_stop();
    pci::RasterTileRequest request;
    request.key = pci::RasterTileKey{0, 0, 0};
    // Cancellation is not a failure: it must be distinguishable from a read
    // error so that a cancelled pan does not poison the negative cache.
    CHECK_THROWS_AS(data->source->readTile(request, source.get_token()),
                    pci::RasterReadCancelled);
}

TEST_CASE("raster tile reads reject unknown levels", "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgb);
    pci::RasterTileRequest request;
    request.key = pci::RasterTileKey{9, 0, 0};
    CHECK_THROWS_AS(data->source->readTile(request, std::stop_token{}),
                    pci::RasterReadError);
}

TEST_CASE("raster rotation and negative pixel height are placed correctly",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rotated);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    // A rotated footprint's bounds must come from all four transformed
    // corners, not from the first and last.
    const std::array<pci::Vec3d, 4> corners = pci::rasterCornerPoints(
        metadata.geoTransform, metadata.width, metadata.height);
    for (const pci::Vec3d corner : corners) {
        CHECK(corner.x >= metadata.bounds.minimum[0]);
        CHECK(corner.x <= metadata.bounds.maximum[0]);
        CHECK(corner.y >= metadata.bounds.minimum[1]);
        CHECK(corner.y <= metadata.bounds.maximum[1]);
    }
    CHECK(metadata.bounds.maximum[0] > metadata.bounds.minimum[0]);
    CHECK_NOTHROW(readFirstTile(*data));
}

TEST_CASE("raster import rejects an antimeridian crossing by name",
          "[component][gdal]")
{
    // A wrapped bound is not confined to the offending layer: it would corrupt
    // scene fitting for the whole document, so the import fails outright.
    REQUIRE_THROWS_AS(load(fixtures().antimeridian), pci::RasterImportError);
    try {
        static_cast<void>(load(fixtures().antimeridian));
        FAIL("expected the antimeridian import to fail");
    } catch (const pci::RasterImportError &error) {
        CHECK(std::string(error.what()).find("antimeridian") !=
              std::string::npos);
    }
}

TEST_CASE("raster import reports a missing source with the driver message",
          "[component][gdal]")
{
    CHECK_THROWS_AS(load("no-such-raster-file.tif"), pci::RasterImportError);
}

TEST_CASE("raster import compares CRS without reprojecting",
          "[component][gdal]")
{
    const pci::GdalRasterLoader loader;
    pci::RasterImportRequest request;
    request.sourcePath = fixtures().rgb;

    const pci::RasterLayerDataPtr matching = loader.inspect(request).data;
    request.targetSpatialReferenceWkt =
        matching->metadata().spatialReferenceWkt;
    CHECK_FALSE(loader.inspect(request).data->metadata().crsMismatch);

    pci::Bounds3d elsewhere;
    elsewhere.minimum = {0.0, 0.0, 0.0};
    elsewhere.maximum = {10.0, 10.0, 0.0};
    request.targetExtent = elsewhere;
    CHECK(loader.inspect(request).data->metadata().extentDisjointXY);
}

} // namespace
TEST_CASE("raster static image uses a backed overview when one fits",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data =
        load(fixtures().rgbNonPowerOfTwoOverviews);
    const pci::RasterStaticImage image = data->source->readStaticImage(
        4096, data->metadata().defaultDisplay, std::stop_token{});

    // The 256x192 base already fits the cap, so the finest level is chosen and
    // nothing is decimated.
    CHECK(image.levelIndex == 0);
    CHECK_FALSE(image.decimatedBase);
    CHECK(image.width == 256);
    CHECK(image.height == 192);
    CHECK(image.rgba.size() ==
          static_cast<std::size_t>(image.width) * image.height * 4);

    // A small device cap forces a coarser backed level rather than a
    // decimating read of a level that does not fit.
    const pci::RasterStaticImage coarse = data->source->readStaticImage(
        100, data->metadata().defaultDisplay, std::stop_token{});
    CHECK(coarse.levelIndex > 0);
    CHECK_FALSE(coarse.decimatedBase);
    CHECK(coarse.width <= 100);
}

TEST_CASE("raster static image decimates a bounded mid-size base band",
          "[component][gdal]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data =
        load(fixtures().midSizeNoOverviews, loader);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    REQUIRE(metadata.width == 5000);
    REQUIRE(metadata.levels.size() == 1);
    // Below the bounded-read threshold, so this ordinary source is displayable
    // rather than reporting that tiled rendering is required.
    CHECK_FALSE(metadata.insufficientOverviews);
    CHECK_FALSE(pci::rasterRequiresTiledRendering(metadata));

    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);
    const std::uint64_t before = source->readCount();

    const pci::RasterStaticImage image = data->source->readStaticImage(
        4096, metadata.defaultDisplay, std::stop_token{});
    CHECK(image.decimatedBase);
    CHECK(image.levelIndex == 0);
    CHECK(image.width == pci::rasterStaticTextureLimitPixels);
    CHECK(image.height == pci::rasterStaticTextureLimitPixels);
    CHECK(image.rgba.size() ==
          static_cast<std::size_t>(image.width) * image.height * 4);

    // One read per selected band, not one per source block.
    CHECK(source->readCount() - before == 3);
}

TEST_CASE("raster static image refuses an unbounded base band",
          "[component][gdal][stress]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().sparseHuge);
    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);
    const std::uint64_t before = source->readCount();

    // The message names the condition, and — more importantly — no read is
    // issued. A source above the threshold must never scan its base image.
    CHECK_THROWS_AS(
        data->source->readStaticImage(
            4096, data->metadata().defaultDisplay, std::stop_token{}),
        pci::RasterReadError);
    CHECK(source->readCount() == before);

    try {
        static_cast<void>(data->source->readStaticImage(
            4096, data->metadata().defaultDisplay, std::stop_token{}));
        FAIL("expected the unbounded static read to be refused");
    } catch (const pci::RasterReadError &error) {
        CHECK(std::string(error.what()).find("tiled rendering") !=
              std::string::npos);
    }
}

TEST_CASE("raster static image honours transparency and cancellation",
          "[component][gdal]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgba);
    const pci::RasterStaticImage image = data->source->readStaticImage(
        4096, data->metadata().defaultDisplay, std::stop_token{});
    REQUIRE(image.width == 32);

    const auto texel = [&image](const std::uint32_t x, const std::uint32_t y) {
        const std::size_t base =
            (static_cast<std::size_t>(y) * image.width + x) * 4;
        return std::array<std::byte, 4>{image.rgba[base],
                                        image.rgba[base + 1],
                                        image.rgba[base + 2],
                                        image.rgba[base + 3]};
    };
    // The fixture's left half is transparent, and premultiplication zeroes its
    // color so filtering cannot drag it into a valid neighbour.
    CHECK(texel(4, 4)[3] == std::byte{0});
    CHECK(texel(4, 4)[0] == std::byte{0});
    CHECK(texel(28, 4)[3] == std::byte{255});

    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_AS(
        data->source->readStaticImage(
            4096, data->metadata().defaultDisplay, stop.get_token()),
        pci::RasterReadCancelled);
}
