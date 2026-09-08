#include "fixtures/GdalRasterFixtureFactory.h"
#include "import/gdal/GdalRasterDataset.h"
#include "import/gdal/GdalRasterLoader.h"
#include "import/gdal/GdalRasterSource.h"
#include "import/gdal/GdalRuntime.h"
#include "import/gdal/GdalSpatialReferenceComparator.h"
#include "raster/RasterPointSampler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path temporaryPath(QTemporaryDir &directory)
{
    if (!directory.isValid()) {
        throw std::runtime_error("could not create private GDAL fixture");
    }
    return QDir(directory.path()).filesystemPath();
}

// The fixture corpus is written once; every case here is read-only.
struct GdalFixtures final {
    GdalFixtures()
        : paths(pci::test::writeGdalRasterFixtures(temporaryPath(directory)))
    {
    }

    QTemporaryDir directory;
    pci::test::GdalRasterFixturePaths paths;
};

const pci::test::GdalRasterFixturePaths &fixtures()
{
    static const GdalFixtures fixture;
    return fixture.paths;
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

[[nodiscard]] pci::RasterTileData
readFirstElevationTile(const pci::RasterLayerData &data,
                       const std::uint32_t level = 0)
{
    pci::RasterTileRequest request;
    request.key = pci::RasterTileKey{level, 0, 0};
    request.decode = std::make_shared<pci::RasterDecodeParameters>(
        data.metadata().defaultDisplay);
    request.profile = pci::RasterTilePayloadProfile::RenderElevation;
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
    CHECK(metadata.nativePixelSize[0] == Catch::Approx(2.0));
    CHECK(metadata.nativePixelSize[1] == Catch::Approx(2.0));
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
        CHECK(data->metadata().elevation.available);
        CHECK(data->metadata().elevation.band == 1);
    }

    SECTION("unlabelled RGB bands preserve a positional warning")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().positionalRgb);
        CHECK(data->metadata().defaultDisplay.sampleKind ==
              pci::RasterSampleKind::ContinuousColor);
        CHECK(data->metadata().positionalBandFallback);
    }
}

TEST_CASE("DEM payload stores residual heights and actual float extrema",
          "[component][gdal][surface]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().terrain);
    const pci::RasterLayerMetadata &metadata = data->metadata();
    REQUIRE(metadata.elevation.available);
    const pci::RasterTileData tile = readFirstElevationTile(*data);
    REQUIRE(tile.profile == pci::RasterTilePayloadProfile::RenderElevation);
    REQUIRE(tile.elevation.size() ==
            pci::rasterStoredTilePixels * pci::rasterStoredTilePixels);
    REQUIRE(tile.hasValidElevation);

    const auto height = [&tile](const std::uint32_t x, const std::uint32_t y) {
        return tile.elevation[static_cast<std::size_t>(y) *
                                  pci::rasterStoredTilePixels +
                              x];
    };
    // Source (40,20) is valid and stored after the one-pixel gutter.
    CHECK(static_cast<double>(height(41, 21)) + metadata.elevation.anchor ==
          Catch::Approx(160.0));
    // Source (4,20) is nodata. Invalid height storage is finite and filled
    // with the tile minimum, while RGBA alpha remains the validity mask.
    CHECK(height(5, 21) == tile.elevationMinimum);
    CHECK(texel(tile, 5, 21)[3] == std::byte{0});
    const auto [minimum, maximum] = std::ranges::minmax(tile.elevation);
    CHECK(minimum == tile.elevationMinimum);
    CHECK(maximum == tile.elevationMaximum);
    CHECK(tile.byteSize() >=
          pci::rasterStoredTileBytes + tile.elevation.size() * sizeof(float));
}

TEST_CASE("DEM exact analysis reports the valid scaled range and progress",
          "[component][gdal][surface]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().terrain);
    std::vector<pci::RasterElevationScanProgress> progress;
    const pci::RasterElevationRange range = data->source->exactElevationRange(
        std::stop_token{},
        [&progress](const pci::RasterElevationScanProgress value) {
            progress.push_back(value);
        });
    CHECK(range.minimum == Catch::Approx(108.0));
    CHECK(range.maximum == Catch::Approx(226.0));
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.front().processedBlocks == 0);
    CHECK(progress.back().processedBlocks == progress.back().totalBlocks);
    CHECK(progress.back().totalBlocks > 0);

    std::stop_source cancelled;
    cancelled.request_stop();
    CHECK_THROWS_AS(
        data->source->exactElevationRange(cancelled.get_token(), {}),
        pci::RasterReadCancelled);
}

TEST_CASE("DEM validity applies explicit alpha before scaled extrema",
          "[component][gdal][surface][alpha]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().terrainAlpha);
    const auto &elevation = data->metadata().elevation;
    REQUIRE(elevation.available);
    CHECK(elevation.scale == -2.0);
    CHECK(elevation.offset == 100.0);
    CHECK(elevation.unit == "m");

    // Valid raw samples span 16..46. Negative scale reverses their order;
    // the transparent half contains much larger raw values and must not leak
    // into either endpoint.
    const pci::RasterElevationRange range =
        data->source->exactElevationRange(std::stop_token{}, {});
    CHECK(range.minimum == Catch::Approx(8.0));
    CHECK(range.maximum == Catch::Approx(68.0));

    const pci::RasterTileData tile = readFirstElevationTile(*data);
    REQUIRE(tile.hasValidElevation);
    CHECK(texel(tile, 5, 5)[3] == std::byte{0});
    const std::size_t invalid = 5U * pci::rasterStoredTilePixels + 5U;
    CHECK(tile.elevation[invalid] == tile.elevationMinimum);
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
    pci::RasterTileRequest request;
    request.key = pci::RasterTileKey{0, 0, 0};
    request.decode = std::make_shared<pci::RasterDecodeParameters>(
        data->metadata().defaultDisplay);
    const std::uint64_t reserved = data->source->readReservationBytes(request);
    const pci::RasterTileData tile =
        data->source->readTile(request, std::stop_token{});
    CHECK(reserved >= tile.byteSize());
    CHECK(reserved <= pci::rasterMaximumTileReadReservationBytes);

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

TEST_CASE("detached raster readers do not consume display handles or counters",
          "[component][gdal][colorize]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().rgb);
    const auto *display =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(display != nullptr);
    const pci::RasterTileSourcePtr detached = data->source->detachedReader(2);
    REQUIRE(detached != nullptr);
    const auto *detachedGdal =
        dynamic_cast<const pci::GdalRasterSource *>(detached.get());
    REQUIRE(detachedGdal != nullptr);

    const pci::RasterTileRequest request{
        .key = {0, 0, 0},
        .renderGeneration = 7,
        .decode = std::make_shared<const pci::RasterDecodeParameters>(
            data->metadata().defaultDisplay),
    };
    const std::uint64_t displayBefore = display->readCount();
    const pci::RasterTileData detachedTile = detached->readTile(request, {});
    CHECK(display->readCount() == displayBefore);
    CHECK(detachedGdal->readCount() == 3);

    const pci::RasterTileData displayTile = data->source->readTile(request, {});
    CHECK(displayTile.validWidth == detachedTile.validWidth);
    CHECK(displayTile.validHeight == detachedTile.validHeight);
    CHECK(displayTile.rgba == detachedTile.rgba);
    CHECK(display->readCount() == displayBefore + 3);
}

TEST_CASE("point-aligned GDAL colors match analytic raster sampling",
          "[component][gdal][colorize]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().pointAligned);
    const auto placement =
        pci::RasterInversePlacement::forMetadata(data->metadata());
    REQUIRE(placement.has_value());
    const pci::RasterTileData tile = readFirstTile(*data);
    constexpr std::array<std::array<std::uint32_t, 2>, 8> pixels{{
        {0, 0},
        {5, 3},
        {10, 2},
        {15, 8},
        {20, 12},
        {25, 20},
        {30, 25},
        {31, 31},
    }};
    for (const auto pixel : pixels) {
        const double worldX = 995.0 + pixel[0] + 0.5;
        const double worldY = 2015.0 - pixel[1] - 0.5;
        const auto address = placement->addressOf(worldX, worldY);
        REQUIRE(address.has_value());
        REQUIRE(address->tile == pci::RasterTileKey{0, 0, 0});
        CHECK(address->innerX == pixel[0]);
        CHECK(address->innerY == pixel[1]);
        const std::uint32_t byteOffset =
            ((pixel[1] + pci::rasterTileGutter) * pci::rasterStoredTilePixels +
             pixel[0] + pci::rasterTileGutter) *
            4U;
        const std::uint32_t expected =
            0xff000000U | (64U << 16U) | (pixel[1] * 8U << 8U) | pixel[0] * 8U;
        CHECK(pci::rasterTexelToPointColor(tile, byteOffset) == expected);
    }
}

TEST_CASE("point-aligned partial alpha round-trips displayed premultiplication",
          "[component][gdal][colorize]")
{
    const pci::RasterLayerDataPtr data =
        load(fixtures().pointAlignedPartialAlpha);
    const pci::RasterTileData tile = readFirstTile(*data);
    for (const std::array<std::uint32_t, 2> pixel :
         {std::array<std::uint32_t, 2>{5, 10},
          std::array<std::uint32_t, 2>{17, 17},
          std::array<std::uint32_t, 2>{29, 25}}) {
        const std::uint32_t byteOffset =
            ((pixel[1] + pci::rasterTileGutter) * pci::rasterStoredTilePixels +
             pixel[0] + pci::rasterTileGutter) *
            4U;
        const auto color = pci::rasterTexelToPointColor(tile, byteOffset);
        REQUIRE(color.has_value());
        CHECK((*color >> 24U) == 0xffU);
        // Integer premultiply/unpremultiply may differ by a small rounding
        // amount; it must remain close to the unassociated source channels.
        CHECK(std::abs(static_cast<int>(*color & 0xffU) -
                       static_cast<int>(pixel[0] * 8U)) <= 3);
        CHECK(std::abs(static_cast<int>((*color >> 8U) & 0xffU) -
                       static_cast<int>(pixel[1] * 8U)) <= 3);
        CHECK(std::abs(static_cast<int>((*color >> 16U) & 0xffU) - 64) <= 3);
    }
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

TEST_CASE("raster tile reads apply an explicit dataset mask",
          "[component][gdal][mask]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().masked);
    REQUIRE(data->metadata().levels.size() == 1);
    REQUIRE(data->metadata().levels.front().maskBand.has_value());
    const pci::RasterTileData tile = readFirstTile(*data);

    const std::array<std::byte, 4> invalid = texel(tile, 4, 20);
    CHECK(invalid[0] == std::byte{0});
    CHECK(invalid[1] == std::byte{0});
    CHECK(invalid[2] == std::byte{0});
    CHECK(invalid[3] == std::byte{0});
    CHECK(texel(tile, 40, 20)[3] == std::byte{255});
}

TEST_CASE("raster levels reject a color overview without a matching mask",
          "[component][gdal][mask][overview]")
{
    const pci::RasterLayerDataPtr data =
        load(fixtures().mismatchedMaskOverviews);
    const pci::RasterLayerMetadata &metadata = data->metadata();
    REQUIRE(metadata.levels.size() == 1);
    CHECK(metadata.levels.front().width == metadata.width);
    CHECK(metadata.levels.front().maskBand.has_value());
    CHECK_THROWS_AS(readFirstTile(*data, 1), pci::RasterReadError);
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

    // This is an actually backed, downsampled level. The transparent side has
    // zero premultiplied color, so sampling cannot bleed terrain color across
    // the overview's nodata boundary.
    REQUIRE(data->metadata().levels.size() >= 2);
    const pci::RasterTileData overview = readFirstTile(*data, 1);
    CHECK(texel(overview, 3, 10)[3] == std::byte{0});
    CHECK(texel(overview, 3, 10)[0] == std::byte{0});
    CHECK(texel(overview, 10, 10)[3] == std::byte{255});
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
    CHECK(metadata.nativePixelSize[0] ==
          Catch::Approx(
              std::hypot(metadata.geoTransform[1], metadata.geoTransform[4])));
    CHECK(metadata.nativePixelSize[1] ==
          Catch::Approx(
              std::hypot(metadata.geoTransform[2], metadata.geoTransform[5])));
    CHECK_NOTHROW(readFirstTile(*data));
}

TEST_CASE("raster placement is derived from a world-file sidecar",
          "[component][gdal][world-file]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().worldFile);
    const pci::RasterLayerMetadata &metadata = data->metadata();
    CHECK(metadata.crsMissing);
    CHECK(metadata.geoTransform[0] == Catch::Approx(674000.0));
    CHECK(metadata.geoTransform[1] == Catch::Approx(2.0));
    CHECK(metadata.geoTransform[2] == Catch::Approx(0.0));
    CHECK(metadata.geoTransform[3] == Catch::Approx(6580000.0));
    CHECK(metadata.geoTransform[4] == Catch::Approx(0.0));
    CHECK(metadata.geoTransform[5] == Catch::Approx(-2.0));
    CHECK(metadata.bounds.minimum[0] == Catch::Approx(674000.0));
    CHECK(metadata.bounds.maximum[0] == Catch::Approx(674032.0));
    CHECK(metadata.bounds.maximum[1] == Catch::Approx(6580000.0));
    CHECK(metadata.bounds.minimum[1] == Catch::Approx(6579976.0));
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

TEST_CASE("GDAL spatial reference comparator distinguishes all relations",
          "[component][gdal][colorize][crs]")
{
    const pci::GdalSpatialReferenceComparator comparator;
    const std::string projected =
        load(fixtures().rgb)->metadata().spatialReferenceWkt;
    const std::string geographic =
        R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])";
    CHECK(comparator.compare(projected, projected) ==
          pci::SpatialReferenceRelation::Same);
    CHECK(comparator.compare(projected, geographic) ==
          pci::SpatialReferenceRelation::Different);
    CHECK(comparator.compare(projected, {}) ==
          pci::SpatialReferenceRelation::Unknown);
    CHECK(comparator.compare(projected, "not a WKT") ==
          pci::SpatialReferenceRelation::Unknown);
}

} // namespace
TEST_CASE("raster mid-size source without overviews renders tiled",
          "[component][gdal]")
{
    // The most common first import: about 5000x5000 with no overviews. The
    // tiled path retains native detail and adds a non-persistent logical
    // coverage pyramid for zoomed-out display.
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data =
        load(fixtures().midSizeNoOverviews, loader);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    REQUIRE(metadata.width == 5000);
    REQUIRE(pci::rasterBackedLevelCount(metadata.levels) == 1);
    REQUIRE(pci::rasterGeneratedLevelCount(metadata.levels) == 5);
    REQUIRE(metadata.levels.back().width == 157);
    REQUIRE(metadata.levels.back().height == 157);
    CHECK_FALSE(metadata.insufficientOverviews);
    CHECK_FALSE(pci::rasterRequiresTiledRendering(metadata));

    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);
    const std::uint64_t before = source->readCount();

    const pci::RasterTileData tile = data->source->readTile(
        pci::RasterTileRequest{
            .key = pci::RasterTileKey{0, 0, 0},
            .renderGeneration = 1,
            .decode = std::make_shared<const pci::RasterDecodeParameters>(
                metadata.defaultDisplay),
        },
        std::stop_token{});
    CHECK(tile.validWidth == pci::rasterTilePixels);
    CHECK(tile.validHeight == pci::rasterTilePixels);
    CHECK(tile.rgba.size() == pci::rasterStoredTileBytes);
    // One window per selected band, sized to the tile rather than the source.
    CHECK(source->readCount() - before == 3);

    const std::uint64_t beforeCoverage = source->readCount();
    const pci::RasterTileData coverage = readFirstTile(
        *data, static_cast<std::uint32_t>(metadata.levels.size() - 1));
    CHECK(coverage.validWidth == 157);
    CHECK(coverage.validHeight == 157);
    CHECK(coverage.rgba.size() == pci::rasterStoredTileBytes);
    CHECK(source->readCount() - beforeCoverage == 3);
}

TEST_CASE("automatic coverage resampling preserves raster sample semantics",
          "[component][gdal][coverage]")
{
    SECTION("continuous samples use a filtered preview")
    {
        const pci::RasterLayerDataPtr data =
            load(fixtures().coverageContinuous);
        REQUIRE(data->metadata().levels.size() == 2);
        REQUIRE(data->metadata().levels.back().kind ==
                pci::RasterLevelKind::GeneratedCoverage);
        const auto sample = texel(readFirstTile(*data, 1), 40, 40);
        const int gray = std::to_integer<int>(sample[0]);
        // The source alternates black/white every pixel. A 2:1 bilinear read
        // is gray; nearest-neighbour would remain at an endpoint.
        CHECK(gray > 80);
        CHECK(gray < 180);
        CHECK(sample[0] == sample[1]);
        CHECK(sample[1] == sample[2]);
        CHECK(sample[3] == std::byte{255});
    }

    SECTION("categorical samples never invent an interpolated class")
    {
        const pci::RasterLayerDataPtr data =
            load(fixtures().coverageCategorical);
        REQUIRE(data->metadata().levels.size() == 2);
        const pci::RasterTileData tile = readFirstTile(*data, 1);
        for (std::uint32_t x = 20; x < 40; ++x) {
            const auto sample = texel(tile, x, 40);
            // Only red (class 0) and blue (class 2) exist in the source. A
            // bilinear average would create the absent green class 1.
            CHECK(sample[1] == std::byte{0});
            CHECK(sample[3] == std::byte{255});
        }
    }

    SECTION("dataset masks remain hard validity boundaries")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().coverageMasked);
        REQUIRE(data->metadata().levels.size() == 2);
        const pci::RasterTileData tile = readFirstTile(*data, 1);
        CHECK(texel(tile, 20, 80)[3] == std::byte{0});
        CHECK(texel(tile, 100, 80)[3] == std::byte{255});
    }

    SECTION("nodata remains transparent in generated levels")
    {
        const pci::RasterLayerDataPtr data = load(fixtures().coverageNodata);
        REQUIRE(data->metadata().levels.size() == 2);
        const pci::RasterTileData tile = readFirstTile(*data, 1);
        CHECK(texel(tile, 20, 80)[3] == std::byte{0});
        CHECK(texel(tile, 100, 80)[3] == std::byte{255});
    }
}

TEST_CASE("raster source above the coverage limit reports missing overviews",
          "[component][gdal][stress]")
{
    const pci::RasterLayerDataPtr data = load(fixtures().sparseHuge);
    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);
    const std::uint64_t before = source->readCount();

    // Covering the view from this source's coarsest level would take
    // thousands of native tiles, so it is flagged. Asking for the flag must
    // itself read nothing: the warning is derived from metadata, never from a
    // scan.
    CHECK(data->metadata().insufficientOverviews);
    CHECK(pci::rasterRequiresTiledRendering(data->metadata()));
    CHECK(source->readCount() == before);
}

TEST_CASE("raster source drivers come from the path, not its contents",
          "[unit][gdal][catalog]")
{
    const auto drivers = [](const char *name) {
        return pci::rasterSourceDriversFor(std::filesystem::path(name));
    };

    CHECK(drivers("mosaic.vrt").container == "VRT");
    CHECK(drivers("mosaic.vrt").index.empty());

    // A tile index names its storage format in its own extension, so the
    // required index driver is known without opening anything. That is what
    // keeps import cost independent of how many members a catalog holds.
    CHECK(drivers("tiles.gti.gpkg").container == "GTI");
    CHECK(drivers("tiles.gti.gpkg").index == "GPKG");
    CHECK(drivers("tiles.gti.fgb").index == "FlatGeobuf");
    CHECK(drivers("tiles.gti.shp").index == "ESRI Shapefile");
    CHECK(drivers("tiles.gti").container == "GTI");
    CHECK(drivers("tiles.gti").index.empty());

    // Case and directories must not change the answer.
    CHECK(drivers("/data/Archive/TILES.GTI.GPKG").index == "GPKG");

    // An ordinary raster implies nothing, so its failures keep reporting the
    // driver's own message.
    CHECK(drivers("scene.tif").container.empty());
    CHECK(drivers("scene.tif").index.empty());
    // A plain GeoPackage raster is not a tile index.
    CHECK(drivers("scene.gpkg").container.empty());
}

TEST_CASE("VRT mosaic exposes the union extent and its common overviews",
          "[component][gdal][catalog]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data = load(fixtures().vrtMosaic, loader);
    const pci::RasterLayerMetadata &metadata = data->metadata();

    CHECK(metadata.sourceDriver == "VRT");
    // Four small members 400 km apart: the mosaic is enormous logically while
    // nothing enormous was written to disk.
    CHECK(metadata.width == 200512);
    CHECK(metadata.height == 200512);
    // The members' own overviews surface as three backed mosaic levels; the
    // remaining entries are the automatic logical coverage pyramid.
    REQUIRE(pci::rasterBackedLevelCount(metadata.levels) == 3);
    REQUIRE(pci::rasterGeneratedLevelCount(metadata.levels) > 0);
    CHECK(metadata.levels[0].width == 200512);
    CHECK(metadata.levels[1].width == 100256);
    CHECK(metadata.levels[2].width == 50128);
    CHECK(metadata.bands.size() == 3);
}

TEST_CASE("GTI catalog opens without enumerating its members",
          "[component][gdal][catalog]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data = load(fixtures().catalog, loader);

    // Inspection cost must come from the sample budget, not from catalog
    // cardinality or pixel count. 200512 squared is about 4e10 pixels; a scan
    // proportional to either would not return in a test.
    CHECK(loader.sampleReadCount() <= 64);

    const pci::RasterLayerMetadata &metadata = data->metadata();
    CHECK(metadata.sourceDriver == "GTI");
    CHECK(metadata.width == 200512);
    CHECK(metadata.height == 200512);
    CHECK(metadata.bands.size() == 3);
}

TEST_CASE("large sources report whether backed overviews can cover cheaply",
          "[component][gdal][catalog]")
{
    const pci::GdalRasterLoader loader;

    // The catalog's fixed external overview reduces whole-source coverage to
    // a bounded tile set, so it is sufficient despite the 40-billion-pixel
    // logical base.
    const pci::RasterLayerDataPtr catalog = load(fixtures().catalog, loader);
    REQUIRE(pci::rasterBackedLevelCount(catalog->metadata().levels) == 2);
    CHECK_FALSE(catalog->metadata().insufficientOverviews);

    // The mosaic does have overviews, and is still flagged. The warning is
    // about whether any level can cover the view cheaply, not about whether
    // overviews exist at all: 50128 pixels still needs about 38000 tiles.
    const pci::RasterLayerDataPtr mosaic = load(fixtures().vrtMosaic, loader);
    REQUIRE(pci::rasterBackedLevelCount(mosaic->metadata().levels) == 3);
    CHECK(mosaic->metadata().insufficientOverviews);
}

TEST_CASE("catalog tile reads reach native member pixels at bounded cost",
          "[component][gdal][catalog]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayerDataPtr data = load(fixtures().catalog, loader);
    const pci::RasterLayerMetadata &metadata = data->metadata();
    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(data->source.get());
    REQUIRE(source != nullptr);

    // The first member sits at the catalog's north-west corner, so tile (0,0)
    // of the native level lands inside real pixels.
    const std::uint64_t before = source->readCount();
    const pci::RasterTileData tile = data->source->readTile(
        pci::RasterTileRequest{
            .key = pci::RasterTileKey{0, 0, 0},
            .renderGeneration = 1,
            .decode = std::make_shared<const pci::RasterDecodeParameters>(
                metadata.defaultDisplay),
        },
        std::stop_token{});

    // One window per selected band, whatever the catalog's size: the read is
    // driven by the tile, not by the index.
    CHECK(source->readCount() - before == 3);
    CHECK(tile.validWidth == pci::rasterTilePixels);
    CHECK(tile.rgba.size() == pci::rasterStoredTileBytes);

    // The member carries a two-pixel checkerboard that no overview could
    // reproduce, so finding both phases proves native pixels were read.
    const auto texel = [&tile](const std::uint32_t x, const std::uint32_t y) {
        const std::size_t index =
            ((static_cast<std::size_t>(y) + 1) * pci::rasterStoredTilePixels +
             static_cast<std::size_t>(x) + 1) *
            4;
        return std::to_integer<int>(tile.rgba[index]);
    };
    CHECK(texel(0, 0) != texel(2, 0));
    CHECK(texel(0, 0) == texel(1, 0));
}

TEST_CASE("a missing driver is named, and the container comes first",
          "[unit][gdal][catalog]")
{
    // Every build the tests run on has all of these drivers, so the absent
    // case is expressed through the capability struct rather than by taking
    // drivers out of GDAL's registry. Doing the latter measures GDAL's
    // deferred-plugin behaviour instead of this decision.
    pci::GdalCatalogCapabilities complete;
    complete.tileIndex = true;
    complete.virtualRaster = true;
    complete.geoPackage = true;
    complete.flatGeobuf = true;
    complete.shapefile = true;

    const pci::RasterSourceDrivers catalog{.container = "GTI", .index = "GPKG"};
    CHECK(pci::missingRasterDriver(catalog, complete).empty());

    // Without GTI the catalog cannot be read at all, so GTI is what the user
    // is told about even though the index driver is also relevant.
    pci::GdalCatalogCapabilities withoutTileIndex = complete;
    withoutTileIndex.tileIndex = false;
    CHECK(pci::missingRasterDriver(catalog, withoutTileIndex) == "GTI");

    // With GTI present, a missing index format is the actionable fact:
    // reinstalling GTI would not help.
    pci::GdalCatalogCapabilities withoutGeoPackage = complete;
    withoutGeoPackage.geoPackage = false;
    CHECK(pci::missingRasterDriver(catalog, withoutGeoPackage) == "GPKG");
    // A catalog indexed differently is unaffected by the same gap.
    CHECK(pci::missingRasterDriver({.container = "GTI", .index = "FlatGeobuf"},
                                   withoutGeoPackage)
              .empty());

    // An ordinary raster implies no driver, so it never reports one missing.
    CHECK(pci::missingRasterDriver({}, pci::GdalCatalogCapabilities{}).empty());
}

TEST_CASE("this build can open the catalog it was given",
          "[component][gdal][catalog]")
{
    // The other direction of the same rule: with every required driver
    // present, the open must actually succeed rather than be refused by an
    // over-eager capability check.
    CHECK(pci::missingRasterDriver(
              pci::rasterSourceDriversFor(fixtures().catalog),
              pci::gdalCatalogCapabilities())
              .empty());
    CHECK(pci::openRasterDataset(fixtures().catalog) != nullptr);
    CHECK(pci::openRasterDataset(fixtures().vrtMosaic) != nullptr);
}
