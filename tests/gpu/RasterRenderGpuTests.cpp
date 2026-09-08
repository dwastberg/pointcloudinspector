#include "app/ApplicationOptions.h"
#include "fixtures/GdalRasterFixtureFactory.h"
#include "import/gdal/GdalRasterLoader.h"
#include "renderer/rhi/RenderViewportWidget_p.h"
#include "scene/PointCloudScene.h"
#include "storage/SecureStorage.h"
#include "support/RenderViewportTestAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

pci::GraphicsApi gpuTestGraphicsApi()
{
    const auto api = pci::parseGraphicsApi(PCINSPECTOR_GPU_TEST_GRAPHICS_API);
    if (!api) {
        throw std::logic_error("invalid configured GPU API");
    }
    return *api;
}

constexpr bool gpuTestValidation = PCINSPECTOR_GPU_TEST_VALIDATION != 0;
constexpr int viewportSize = 256;

// The synthetic source spans this world rectangle, so a top-down frame puts
// its west edge on the left of the screen and its north edge at the top.
constexpr double rasterWest = -100.0;
constexpr double rasterEast = 100.0;
constexpr double rasterSouth = -100.0;
constexpr double rasterNorth = 100.0;

struct Rgba {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

// What each level paints, as a function of base-pixel position. Distinct
// per-level appearance is what makes "coarse coverage first, native detail
// after zooming" observable rather than assumed.
using LevelPainter = std::function<Rgba(
    std::uint32_t levelIndex, double basePixelX, double basePixelY)>;

struct GdalFixtures final {
    GdalFixtures()
        : directory(std::filesystem::temp_directory_path(), "pci-raster-gpu")
        , paths(pci::test::writeGdalRasterFixtures(directory.path()))
    {
    }

    pci::PrivateTemporaryDirectory directory;
    pci::test::GdalRasterFixturePaths paths;
};

const pci::test::GdalRasterFixturePaths &gdalFixtures()
{
    static const GdalFixtures fixture;
    return fixture.paths;
}

class RecordingRasterSource final : public pci::RasterTileSource {
public:
    explicit RecordingRasterSource(pci::RasterTileSourcePtr source)
        : source_(std::move(source))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return source_->metadata();
    }

    [[nodiscard]] std::uint64_t
    readReservationBytes(const pci::RasterTileRequest &request) const override
    {
        return source_->readReservationBytes(request);
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             std::stop_token stop) const override
    {
        {
            const std::scoped_lock lock(mutex_);
            levels_.push_back(request.key.levelIndex);
        }
        return source_->readTile(request, stop);
    }

    [[nodiscard]] std::vector<std::uint32_t> levels() const
    {
        const std::scoped_lock lock(mutex_);
        return levels_;
    }

    void clearLevels()
    {
        const std::scoped_lock lock(mutex_);
        levels_.clear();
    }

private:
    pci::RasterTileSourcePtr source_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint32_t> levels_;
};

class PatternSource final : public pci::RasterTileSource {
public:
    PatternSource(pci::RasterLayerMetadata metadata,
                  LevelPainter painter,
                  const std::chrono::milliseconds readDelay =
                      std::chrono::milliseconds{0})
        : metadata_(std::move(metadata))
        , painter_(std::move(painter))
        , readDelay_(readDelay)
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] std::uint64_t
    readReservationBytes(const pci::RasterTileRequest &request) const override
    {
        const std::uint64_t elevationBytes =
            request.profile == pci::RasterTilePayloadProfile::RenderElevation
                ? static_cast<std::uint64_t>(pci::rasterStoredTilePixels) *
                      pci::rasterStoredTilePixels * sizeof(float)
                : 0;
        return sizeof(pci::RasterTileData) + pci::rasterStoredTileBytes +
               elevationBytes;
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled{};
        }
        ++reads;
        if (readDelay_.count() > 0) {
            std::this_thread::sleep_for(readDelay_);
            if (stop.stop_requested()) {
                throw pci::RasterReadCancelled{};
            }
        }
        if (request.key.levelIndex >= metadata_.levels.size()) {
            throw pci::RasterReadError("unknown level");
        }
        const pci::RasterLevel &level =
            metadata_.levels[request.key.levelIndex];
        const pci::RasterTileExtent extent =
            pci::rasterTileValidExtent(level, request.key);
        if (extent.width == 0 || extent.height == 0) {
            throw pci::RasterReadError("tile outside level");
        }

        pci::RasterTileData tile;
        tile.key = request.key;
        tile.renderGeneration = request.renderGeneration;
        tile.profile = request.profile;
        tile.validWidth = static_cast<std::uint16_t>(extent.width);
        tile.validHeight = static_cast<std::uint16_t>(extent.height);
        tile.rgba.assign(pci::rasterStoredTileBytes, std::byte{0});
        if (request.profile == pci::RasterTilePayloadProfile::RenderElevation) {
            tile.elevation.assign(
                static_cast<std::size_t>(pci::rasterStoredTilePixels) *
                    pci::rasterStoredTilePixels,
                0.0F);
            tile.hasValidElevation = true;
        }

        const auto texelOrigin = [&](const std::uint32_t index,
                                     const double perTexel,
                                     const std::uint32_t tileIndex) {
            return (static_cast<double>(tileIndex) * pci::rasterTilePixels +
                    static_cast<double>(index)) *
                   perTexel;
        };

        // Fill the guttered buffer including its border, replicating the edge
        // texel where the gutter falls outside the level, exactly as the GDAL
        // source does.
        for (std::int32_t y = -1; y <= static_cast<std::int32_t>(extent.height);
             ++y) {
            const std::uint32_t clampedY =
                static_cast<std::uint32_t>(std::clamp<std::int32_t>(
                    y, 0, static_cast<std::int32_t>(extent.height - 1)));
            for (std::int32_t x = -1;
                 x <= static_cast<std::int32_t>(extent.width);
                 ++x) {
                const std::uint32_t clampedX =
                    static_cast<std::uint32_t>(std::clamp<std::int32_t>(
                        x, 0, static_cast<std::int32_t>(extent.width - 1)));
                const Rgba color = painter_(
                    request.key.levelIndex,
                    texelOrigin(
                        clampedX, level.basePixelsPerTexelX, request.key.x),
                    texelOrigin(
                        clampedY, level.basePixelsPerTexelY, request.key.y));
                const std::size_t index = (static_cast<std::size_t>(y + 1) *
                                               pci::rasterStoredTilePixels +
                                           static_cast<std::size_t>(x + 1)) *
                                          4;
                // Premultiplied, matching the decode contract.
                const auto premultiply = [&](const std::uint8_t channel) {
                    return static_cast<std::byte>(
                        static_cast<std::uint32_t>(channel) * color.alpha /
                        255U);
                };
                tile.rgba[index] = premultiply(color.red);
                tile.rgba[index + 1] = premultiply(color.green);
                tile.rgba[index + 2] = premultiply(color.blue);
                tile.rgba[index + 3] = static_cast<std::byte>(color.alpha);
                if (!tile.elevation.empty()) {
                    const float residual = static_cast<float>(
                        50.0 * clampedX /
                        std::max<std::uint32_t>(1, metadata_.width - 1));
                    tile.elevation[index / 4] = residual;
                    tile.elevationMaximum =
                        std::max(tile.elevationMaximum, residual);
                }
            }
        }
        return tile;
    }

    mutable std::atomic_int reads{0};

private:
    pci::RasterLayerMetadata metadata_;
    LevelPainter painter_;
    std::chrono::milliseconds readDelay_;
};

[[nodiscard]] pci::RasterLayerMetadata
patternMetadata(const std::vector<std::uint32_t> &levelWidths)
{
    const std::uint32_t base = levelWidths.front();
    pci::RasterLayerMetadata metadata;
    metadata.width = base;
    metadata.height = base;
    // North-up: positive x step east, negative y step south, origin at the
    // north-west corner.
    metadata.geoTransform = {
        rasterWest,
        (rasterEast - rasterWest) / base,
        0.0,
        rasterNorth,
        0.0,
        -(rasterNorth - rasterSouth) / base,
    };
    metadata.bounds = {
        .minimum = {rasterWest, rasterSouth, 0.0},
        .maximum = {rasterEast, rasterNorth, 0.0},
    };
    for (const std::uint32_t width : levelWidths) {
        pci::RasterLevel level;
        level.width = width;
        level.height = width;
        level.channelCount = 3;
        level.basePixelsPerTexelX = static_cast<double>(base) / width;
        level.basePixelsPerTexelY = static_cast<double>(base) / width;
        metadata.levels.push_back(level);
    }
    return metadata;
}

[[nodiscard]] pci::RasterLayerDataPtr patternLayer(
    const std::vector<std::uint32_t> &levelWidths,
    LevelPainter painter,
    std::shared_ptr<PatternSource> *sourceOut = nullptr,
    const std::chrono::milliseconds readDelay = std::chrono::milliseconds{0})
{
    auto source = std::make_shared<PatternSource>(
        patternMetadata(levelWidths), std::move(painter), readDelay);
    if (sourceOut) {
        *sourceOut = source;
    }
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::move(source),
    });
}

[[nodiscard]] pci::RasterLayerDataPtr
patternLayer(pci::RasterLayerMetadata metadata, LevelPainter painter)
{
    auto source = std::make_shared<PatternSource>(std::move(metadata),
                                                  std::move(painter));
    return std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::move(source),
    });
}

[[nodiscard]] pci::PointCloudScenePtr coplanarPointGrid()
{
    constexpr int cells = 21;
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = cells * cells;
    metadata.sourceBounds = {
        .minimum = {-90.0, -90.0, -0.01},
        .maximum = {90.0, 90.0, 0.01},
    };
    metadata.hasColor = true;
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {-100.0, -100.0, -1.0};
    block->scale = 200.0 / 65535.0;
    block->bounds = metadata.sourceBounds;
    block->points.reserve(cells * cells);
    for (int y = 0; y < cells; ++y) {
        for (int x = 0; x < cells; ++x) {
            const pci::Vec3d position{-90.0 + 180.0 * x / (cells - 1),
                                      -90.0 + 180.0 * y / (cells - 1),
                                      0.0};
            const auto quantized =
                pci::quantizeToBlock(position, block->origin, block->scale);
            block->points.push_back({.x = quantized[0],
                                     .y = quantized[1],
                                     .z = quantized[2],
                                     .rgba = 0xffffffffU});
        }
    }
    block->attributes.resize(block->points.size());
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

QImage renderFrames(pci::RenderViewportWidget &viewport, const int frames)
{
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    QImage image;
    for (int index = 0; index < frames; ++index) {
        const std::uint64_t previous =
            pci::testAccess(viewport).renderedFrameCountForTesting();
        viewport.requestRender();
        REQUIRE(QTest::qWaitFor(
            [&] {
                return pci::testAccess(viewport)
                           .renderedFrameCountForTesting() > previous;
            },
            2000));
        image = viewport.grabFramebuffer();
    }
    REQUIRE_FALSE(image.isNull());
    return image;
}

void renderFramesWithoutReadback(pci::RenderViewportWidget &viewport,
                                 const int frames)
{
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    for (int index = 0; index < frames; ++index) {
        const std::uint64_t previous =
            pci::testAccess(viewport).renderedFrameCountForTesting();
        viewport.requestRender();
        REQUIRE(QTest::qWaitFor(
            [&] {
                return pci::testAccess(viewport)
                           .renderedFrameCountForTesting() > previous;
            },
            2000));
    }
}

// Streaming is asynchronous, so a single frame proves nothing. Render until
// the predicate holds or the budget runs out.
bool renderUntil(pci::RenderViewportWidget &viewport,
                 const std::function<bool(const QImage &)> &ready,
                 const int maximumFrames = 60)
{
    for (int index = 0; index < maximumFrames; ++index) {
        if (ready(renderFrames(viewport, 1))) {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

// Where the layer actually landed. Framing depends on scene bounds and the
// camera's own margins, so tests locate the drawn footprint and sample inside
// it rather than assuming it fills a particular part of the viewport.
struct Footprint {
    int minimumX = 0;
    int maximumX = -1;
    int minimumY = 0;
    int maximumY = -1;

    [[nodiscard]] bool valid() const noexcept
    {
        return maximumX >= minimumX && maximumY >= minimumY;
    }
    [[nodiscard]] int width() const noexcept
    {
        return maximumX - minimumX + 1;
    }
    [[nodiscard]] int height() const noexcept
    {
        return maximumY - minimumY + 1;
    }
};

[[nodiscard]] bool background(const QColor &color)
{
    return color.red() < 40 && color.green() < 40 && color.blue() < 40;
}

[[nodiscard]] Footprint renderedFootprint(const QImage &image)
{
    Footprint footprint{image.width(), -1, image.height(), -1};
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (background(image.pixelColor(x, y))) {
                continue;
            }
            footprint.minimumX = std::min(footprint.minimumX, x);
            footprint.maximumX = std::max(footprint.maximumX, x);
            footprint.minimumY = std::min(footprint.minimumY, y);
            footprint.maximumY = std::max(footprint.maximumY, y);
        }
    }
    return footprint;
}

[[nodiscard]] QColor sampleWithin(const QImage &image,
                                  const Footprint &footprint,
                                  const double fractionX,
                                  const double fractionY)
{
    const int x =
        footprint.minimumX + static_cast<int>(footprint.width() * fractionX);
    const int y =
        footprint.minimumY + static_cast<int>(footprint.height() * fractionY);
    return image.pixelColor(std::clamp(x, 0, image.width() - 1),
                            std::clamp(y, 0, image.height() - 1));
}

bool nearlyRed(const QColor &color)
{
    return color.red() > 150 && color.green() < 90 && color.blue() < 90;
}

bool nearlyGreen(const QColor &color)
{
    return color.green() > 150 && color.red() < 90 && color.blue() < 90;
}

bool nearlyBlue(const QColor &color)
{
    return color.blue() > 150 && color.red() < 90 && color.green() < 90;
}

bool nearlyYellow(const QColor &color)
{
    return color.red() > 150 && color.green() > 150 && color.blue() < 90;
}

bool nearlyWhite(const QColor &color)
{
    return color.red() > 150 && color.green() > 150 && color.blue() > 150;
}

std::uint64_t countWhere(const QImage &image,
                         const std::function<bool(const QColor &)> &predicate)
{
    std::uint64_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (predicate(image.pixelColor(x, y))) {
                ++count;
            }
        }
    }
    return count;
}

void frameWholeScene(pci::RenderViewportWidget &viewport)
{
    viewport.frameVisibleLayersTopDown();
}

[[nodiscard]] pci::VectorLayerDataPtr
filledVectorRectangle(const float minimumX,
                      const float minimumY,
                      const float maximumX,
                      const float maximumY)
{
    auto data = std::make_shared<pci::VectorLayerData>();
    data->origin = {};
    data->fillBatches.push_back({
        .vertices =
            {
                {minimumX, minimumY},
                {maximumX, minimumY},
                {maximumX, maximumY},
                {minimumX, maximumY},
            },
        .indices = {0, 1, 2, 0, 2, 3},
    });
    data->bounds = {
        .minimum = {minimumX, minimumY, 0.0},
        .maximum = {maximumX, maximumY, 0.0},
    };
    data->summary.polygonParts = 1;
    data->featureCount = 1;
    return data;
}

[[nodiscard]] pci::VectorLayerStyle greenVectorFill()
{
    pci::VectorLayerStyle style;
    style.fill = {0.0F, 1.0F, 0.0F, 1.0F};
    style.stroke.alpha = 0.0F;
    style.marker.alpha = 0.0F;
    style.opacity = 1.0F;
    style.alwaysOnTop = true;
    return style;
}

[[nodiscard]] std::unique_ptr<pci::RenderViewportWidget>
makeViewport(const QSize size = QSize(viewportSize, viewportSize))
{
    auto viewport = std::make_unique<pci::RenderViewportWidget>(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport->resize(size);
    return viewport;
}

} // namespace

TEST_CASE("GPU raster streaming wakes an otherwise idle viewport",
          "[gpu][raster][streaming]")
{
    std::shared_ptr<PatternSource> source;
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(patternLayer(
        {256},
        [](std::uint32_t, double, double) {
            return Rgba{0, 0, 255};
        },
        &source,
        std::chrono::milliseconds{300})));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);
    viewport->show();
    REQUIRE(QTest::qWaitForWindowExposed(viewport.get(), 2000));

    // The initial frame starts a deliberately delayed worker read. From this
    // point onward the test only services the Qt event loop: completion must
    // wake the viewport and upload progress must schedule the draw frame.
    REQUIRE(QTest::qWaitFor(
        [&] {
            return source->reads.load() > 0 &&
                   pci::testAccess(*viewport).renderedFrameCountForTesting() >
                       0;
        },
        2000));
    const std::uint64_t initialFrame =
        pci::testAccess(*viewport).renderedFrameCountForTesting();

    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(*viewport).rasterResidentTilesForTesting() >
                       0 &&
                   pci::testAccess(*viewport).rasterDrawnTilesForTesting() > 0;
        },
        5000));
    CHECK(pci::testAccess(*viewport).renderedFrameCountForTesting() >=
          initialFrame + 2);
    CHECK(countWhere(viewport->grabFramebuffer(), nearlyBlue) > 1000);
}

TEST_CASE("GPU Surface uploads and draws an R32F height field",
          "[gpu][raster][surface]")
{
    pci::RasterLayerMetadata metadata = patternMetadata({256});
    metadata.defaultDisplay.sampleKind =
        pci::RasterSampleKind::ContinuousScalar;
    metadata.elevation = {
        .available = true,
        .band = 1,
        .scale = 1.0,
        .offset = 0.0,
        .unit = "m",
        .anchor = 1000.0,
        .cachedExactRange =
            pci::RasterElevationRange{.minimum = 1000.0, .maximum = 1050.0},
    };

    auto document = std::make_shared<pci::SceneDocument>();
    const pci::SceneLayerId layer = document->addRasterLayer(patternLayer(
        std::move(metadata), [](std::uint32_t, double, double) -> Rgba {
            return {220, 160, 40};
        }));
    pci::RasterLayerStyle style = document->rasterLayer(layer)->style;
    style.renderMode = pci::RasterRenderMode::Surface;
    style.surfaceShadingStrength = 0.0F;
    REQUIRE(document->setRasterLayerStyle(layer, style));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);

    REQUIRE(renderUntil(*viewport, [&](const QImage &image) {
        return pci::testAccess(*viewport).rasterSurfaceDrawnTilesForTesting() >
                   0 &&
               renderedFootprint(image).valid();
    }));
    CHECK(viewport->rasterSurfaceCapability() ==
          pci::RasterSurfaceCapability::Supported);
    CHECK(pci::testAccess(*viewport).rasterHeightGpuBytesForTesting() > 0);
}

TEST_CASE("GPU Surface 4K grid characterization",
          "[.benchmark][gpu][raster][surface-4k]")
{
    constexpr std::uint32_t rasterPixels = 2816; // 11 x 11 native tiles
    pci::RasterLayerMetadata metadata =
        patternMetadata({rasterPixels, 704, 176});
    metadata.defaultDisplay.sampleKind =
        pci::RasterSampleKind::ContinuousScalar;
    metadata.elevation = {
        .available = true,
        .band = 1,
        .unit = "m",
        .anchor = 1000.0,
        .cachedExactRange =
            pci::RasterElevationRange{.minimum = 1000.0, .maximum = 1050.0},
    };

    auto document = std::make_shared<pci::SceneDocument>();
    const pci::SceneLayerId layer = document->addRasterLayer(patternLayer(
        std::move(metadata), [](std::uint32_t, double, double) -> Rgba {
            return {220, 160, 40};
        }));
    pci::RasterLayerStyle style = document->rasterLayer(layer)->style;
    style.renderMode = pci::RasterRenderMode::Surface;
    style.surfaceShadingStrength = 1.0F;
    REQUIRE(document->setRasterLayerStyle(layer, style));

    const qreal devicePixelRatio =
        QGuiApplication::primaryScreen() != nullptr
            ? QGuiApplication::primaryScreen()->devicePixelRatio()
            : 1.0;
    const QSize logicalSize(
        static_cast<int>(std::ceil(3840.0 / devicePixelRatio)),
        static_cast<int>(std::ceil(2160.0 / devicePixelRatio)));
    auto viewport = makeViewport(logicalSize);
    // Top-level widgets are otherwise capped to a fraction of the desktop on
    // first show, which would silently turn this into a sub-4K workload.
    viewport->setFixedSize(logicalSize);
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);

    bool warmed = false;
    for (int frame = 0; frame < 180 && !warmed; ++frame) {
        renderFramesWithoutReadback(*viewport, 1);
        warmed =
            pci::testAccess(*viewport).rasterSurfaceDrawnTilesForTesting() >=
            100;
        QTest::qWait(5);
    }
    REQUIRE(warmed);

    constexpr int measuredFrames = 30;
    QElapsedTimer timer;
    timer.start();
    renderFramesWithoutReadback(*viewport, measuredFrames);
    const double averageMilliseconds =
        static_cast<double>(timer.nsecsElapsed()) /
        (1'000'000.0 * measuredFrames);
    const QImage finalImage = viewport->grabFramebuffer();
    const std::size_t drawnTiles =
        pci::testAccess(*viewport).rasterSurfaceDrawnTilesForTesting();
    const std::uint64_t triangles = static_cast<std::uint64_t>(drawnTiles) *
                                    pci::rasterSurfaceGridCellsPerSide *
                                    pci::rasterSurfaceGridCellsPerSide * 2ULL;
    qInfo("Surface 4K: %dx%d, %zu tiles, %llu triangles, %.3f ms/frame",
          finalImage.width(),
          finalImage.height(),
          drawnTiles,
          static_cast<unsigned long long>(triangles),
          averageMilliseconds);

    CHECK(finalImage.width() >= 3840);
    CHECK(finalImage.height() >= 2160);
    CHECK(drawnTiles >= 100);
    CHECK(pci::testAccess(*viewport).rasterHeightGpuBytesForTesting() >
          25ULL * 1024 * 1024);
}

TEST_CASE("GPU raster resources recreate from decoded tiles without rereading",
          "[gpu][raster][lifecycle]")
{
    std::shared_ptr<PatternSource> source;
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(patternLayer(
        {256},
        [](std::uint32_t, double, double) {
            return Rgba{255, 0, 0};
        },
        &source)));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        return countWhere(image, nearlyRed) > 1000;
    }));
    const int readsBeforeRelease = source->reads.load();
    REQUIRE(readsBeforeRelease > 0);

    pci::testAccess(*viewport).releaseResourcesForTesting();
    CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() == 0);
    CHECK(pci::testAccess(*viewport).rasterResidentTilesForTesting() == 0);
    CHECK(pci::testAccess(*viewport).rasterDrawnTilesForTesting() == 0);

    const std::uint64_t releasedFrame =
        pci::testAccess(*viewport).renderedFrameCountForTesting();
    viewport->requestRender();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(*viewport).rasterResidentTilesForTesting() >
                       0 &&
                   pci::testAccess(*viewport).rasterDrawnTilesForTesting() > 0;
        },
        5000));
    CHECK(pci::testAccess(*viewport).renderedFrameCountForTesting() >=
          releasedFrame + 2);
    CHECK(source->reads.load() == readsBeforeRelease);
    CHECK(countWhere(viewport->grabFramebuffer(), nearlyRed) > 1000);
}

TEST_CASE("GPU GTI catalog covers from an overview then reaches native pixels",
          "[gpu][raster][catalog][stress]")
{
    pci::GdalRasterLoader loader;
    pci::RasterImportRequest request;
    request.sourcePath = gdalFixtures().catalog;
    const pci::RasterImportPreflight inspected = loader.inspect(request);
    REQUIRE(inspected.data);
    REQUIRE(inspected.data->metadata().levels.size() > 1);

    auto recording =
        std::make_shared<RecordingRasterSource>(inspected.data->source);
    auto data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = recording,
    });
    const pci::RasterLayerMetadata &metadata = data->metadata();
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(std::move(data)));

    constexpr std::uint64_t cpuBudget = 32ULL * 1024 * 1024;
    constexpr std::uint64_t gpuBudget = 5ULL * 1024 * 1024;
    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), false);
    viewport->setEyeDomeLightingEnabled(false);
    viewport->setRasterByteBudgets(cpuBudget, gpuBudget);

    // The north-west member is fixed at 512 pixels square with 2 m pixels.
    // Start far enough away that only non-native overview/coverage detail is
    // appropriate.
    const pci::Vec3d memberCenter{metadata.bounds.minimum[0] + 512.0,
                                  metadata.bounds.maximum[1] - 512.0,
                                  0.0};
    pci::testAccess(*viewport).frameTopDownAtForTesting(
        memberCenter, 1500.0, 4.0);
    const bool overviewVisible =
        renderUntil(*viewport, [](const QImage &image) {
            return countWhere(image, [](const QColor &color) {
                       return !background(color);
                   }) > 100;
        });
    const QImage overviewImage = viewport->grabFramebuffer();
    const std::uint64_t overviewPixels =
        countWhere(overviewImage, [](const QColor &color) {
            return !background(color);
        });
    const std::vector<std::uint32_t> overviewReads = recording->levels();
    CAPTURE(overviewPixels, overviewReads);
    REQUIRE(overviewVisible);
    REQUIRE_FALSE(overviewReads.empty());
    CHECK(std::ranges::none_of(overviewReads, [](const std::uint32_t level) {
        return level == 0;
    }));
    CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() <= gpuBudget);
    CHECK(pci::testAccess(*viewport).rasterGpuPeakBytesForTesting() <=
          gpuBudget);

    // At native scale the member's 2x2 checkerboard is visible as distinct
    // dark and bright pixels. Its averaged overview is uniform, so this also
    // proves the level-0 read made it through upload and draw.
    recording->clearLevels();
    pci::testAccess(*viewport).frameTopDownAtForTesting(
        memberCenter, 1500.0, 0.10);
    REQUIRE(renderUntil(*viewport, [&](const QImage &image) {
        const std::vector<std::uint32_t> levels = recording->levels();
        const bool readNative = std::ranges::find(levels, 0U) != levels.end();
        const std::uint64_t dark = countWhere(image, [](const QColor &color) {
            return color.red() >= 30 && color.red() < 100 &&
                   color.green() >= 30 && color.green() < 100 &&
                   color.blue() >= 30 && color.blue() < 100;
        });
        return readNative && dark > 100 && countWhere(image, nearlyWhite) > 100;
    }));
    CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() <= gpuBudget);
    CHECK(pci::testAccess(*viewport).rasterGpuPeakBytesForTesting() <=
          gpuBudget);
}

TEST_CASE("GPU raster pixels land at their affine georeferenced coordinates",
          "[gpu][raster][placement]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 256;
    metadata.height = 256;
    // A rotated and skewed parallelogram. Its four corners are deliberately
    // asymmetric so a center-only or north-up placement can not pass.
    metadata.geoTransform = {
        -80.0,
        200.0 / 256.0,
        40.0 / 256.0,
        60.0,
        50.0 / 256.0,
        -160.0 / 256.0,
    };
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.levels = {
        pci::RasterLevel{.width = metadata.width,
                         .height = metadata.height,
                         .basePixelsPerTexelX = 1.0,
                         .basePixelsPerTexelY = 1.0,
                         .channelCount = 3},
    };

    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(patternLayer(
        metadata,
        [width = metadata.width, height = metadata.height](
            std::uint32_t, const double pixel, const double line) -> Rgba {
            if (pixel < width * 0.5 && line < height * 0.5) {
                return {255, 0, 0};
            }
            if (pixel >= width * 0.5 && line < height * 0.5) {
                return {0, 255, 0};
            }
            return pixel < width * 0.5 ? Rgba{0, 0, 255} : Rgba{255, 255, 0};
        })));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    viewport->setOrthographic(true);
    frameWholeScene(*viewport);

    const auto screenAtBasePixel = [&metadata, &viewport](const QSize imageSize,
                                                          const double pixel,
                                                          const double line) {
        const pci::RasterQuadTransform quad = pci::rasterTileQuadTransform(
            metadata,
            {},
            pci::RasterTileKey{0, 0, 0},
            pci::testAccess(*viewport).cameraForTesting().position());
        QMatrix4x4 model;
        model.setColumn(0,
                        QVector4D(static_cast<float>(quad.edgeU.x),
                                  static_cast<float>(quad.edgeU.y),
                                  0.0F,
                                  0.0F));
        model.setColumn(1,
                        QVector4D(static_cast<float>(quad.edgeV.x),
                                  static_cast<float>(quad.edgeV.y),
                                  0.0F,
                                  0.0F));
        model.setColumn(2, QVector4D(0.0F, 0.0F, 1.0F, 0.0F));
        model.setColumn(3,
                        QVector4D(static_cast<float>(quad.origin.x),
                                  static_cast<float>(quad.origin.y),
                                  static_cast<float>(quad.origin.z),
                                  1.0F));
        const QMatrix4x4 mvp =
            pci::testAccess(*viewport).frameViewProjectionForTesting() * model;
        const QVector4D local(static_cast<float>(pixel / metadata.width),
                              static_cast<float>(line / metadata.height),
                              0.0F,
                              1.0F);
        const QVector4D clip = mvp * local;
        const double ndcX = clip.x() / clip.w();
        const double ndcY = clip.y() / clip.w();
        const int x = static_cast<int>(
            std::lround((ndcX + 1.0) * 0.5 * imageSize.width()));
        const int y = static_cast<int>(
            std::lround((1.0 - ndcY) * 0.5 * imageSize.height()));
        return QPoint{x, y};
    };
    const auto sampleAtBasePixel = [&screenAtBasePixel](const QImage &image,
                                                        const double pixel,
                                                        const double line) {
        const QPoint position = screenAtBasePixel(image.size(), pixel, line);
        return image.pixelColor(
            std::clamp(position.x(), 0, image.width() - 1),
            std::clamp(position.y(), 0, image.height() - 1));
    };

    const bool placed = renderUntil(*viewport, [&](const QImage &image) {
        return nearlyRed(sampleAtBasePixel(image, 64.0, 64.0)) &&
               nearlyGreen(sampleAtBasePixel(image, 192.0, 64.0)) &&
               nearlyBlue(sampleAtBasePixel(image, 64.0, 192.0)) &&
               nearlyYellow(sampleAtBasePixel(image, 192.0, 192.0));
    });
    REQUIRE(placed);
}

TEST_CASE("GPU raster shows coarse coverage before native detail",
          "[gpu][raster][lod]")
{
    // The coarse level is uniformly blue; only the native level carries the
    // red/green pattern. What is on screen therefore names the level in use.
    std::shared_ptr<PatternSource> source;
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(patternLayer(
        {2048, 256},
        [](const std::uint32_t levelIndex,
           const double basePixelX,
           const double basePixelY) -> Rgba {
            if (levelIndex > 0) {
                return {0, 0, 255};
            }
            const bool even = (static_cast<int>(basePixelX / 8.0) +
                               static_cast<int>(basePixelY / 8.0)) %
                                  2 ==
                              0;
            return even ? Rgba{255, 0, 0} : Rgba{0, 255, 0};
        },
        &source)));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);

    // Zoomed out, one native texel is far below a screen pixel, so the coarse
    // level covers the view.
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        return countWhere(image, nearlyBlue) > 1000;
    }));

    // Zooming in must reach the source's own pixels rather than magnifying
    // the overview forever.
    pci::testAccess(*viewport).frameTopDownForTesting(0.02);
    static_cast<void>(renderFrames(*viewport, 1));
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        return countWhere(image, nearlyRed) > 200 &&
               countWhere(image, nearlyGreen) > 200;
    }));
    CHECK(source->reads.load() > 0);
}

TEST_CASE("GPU four 10k rasters show complete automatic previews",
          "[gpu][raster][lod][coverage]")
{
    auto document = std::make_shared<pci::SceneDocument>();
    const auto addQuadrant =
        [&document](const double west, const double north, const Rgba color) {
            pci::RasterLayerMetadata metadata = patternMetadata({10000});
            metadata.geoTransform = {
                west,
                100.0 / metadata.width,
                0.0,
                north,
                0.0,
                -100.0 / metadata.height,
            };
            metadata.bounds = *pci::rasterPixelEdgeBounds(
                metadata.geoTransform, metadata.width, metadata.height);
            pci::appendGeneratedRasterCoverageLevels(
                metadata.levels, metadata.width, metadata.height);
            REQUIRE(metadata.levels.back().width <= pci::rasterTilePixels);
            static_cast<void>(document->addRasterLayer(patternLayer(
                std::move(metadata), [color](std::uint32_t, double, double) {
                    return color;
                })));
        };
    addQuadrant(-100.0, 100.0, {255, 0, 0});
    addQuadrant(0.0, 100.0, {0, 255, 0});
    addQuadrant(-100.0, 0.0, {0, 0, 255});
    addQuadrant(0.0, 0.0, {255, 255, 0});

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    viewport->setRasterByteBudgets(16ULL * 1024 * 1024, 8ULL * 1024 * 1024);
    frameWholeScene(*viewport);

    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        return countWhere(image, nearlyRed) > 300 &&
               countWhere(image, nearlyGreen) > 300 &&
               countWhere(image, nearlyBlue) > 300 &&
               countWhere(image, nearlyYellow) > 300;
    }));
    CHECK(pci::testAccess(*viewport).rasterDrawnTilesForTesting() >= 4);
    CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() <=
          8ULL * 1024 * 1024);
}

TEST_CASE("GPU raster layers obey painter order and opacity",
          "[gpu][raster][order]")
{
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(
        patternLayer({256}, [](std::uint32_t, double, double) {
            return Rgba{255, 0, 0};
        })));
    const pci::SceneLayerId top = document->addRasterLayer(
        patternLayer({1024, 256}, [](std::uint32_t, double, double) {
            return Rgba{0, 255, 0};
        }));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);

    // The later layer wins where they overlap.
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        const Footprint footprint = renderedFootprint(image);
        return footprint.valid() &&
               nearlyGreen(sampleWithin(image, footprint, 0.5, 0.5));
    }));

    // At half opacity the layer beneath shows through, so neither channel
    // dominates.
    pci::RasterLayerStyle style;
    style.opacity = 0.5F;
    static_cast<void>(document->setRasterLayerStyle(top, style));
    viewport->updateDocument(document->snapshot());
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        const Footprint footprint = renderedFootprint(image);
        if (!footprint.valid()) {
            return false;
        }
        const QColor color = sampleWithin(image, footprint, 0.5, 0.5);
        return color.red() > 60 && color.green() > 60;
    }));
}

TEST_CASE("GPU coplanar points remain visible over rasters through an EDL "
          "camera sweep",
          "[gpu][raster][edl][depth]")
{
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addLayer(coplanarPointGrid()));
    static_cast<void>(document->addRasterLayer(
        patternLayer({256}, [](std::uint32_t, double, double) {
            return Rgba{0, 0, 180};
        })));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setPointSizePixels(5);

    for (const bool edl : {false, true}) {
        viewport->setEyeDomeLightingEnabled(edl);
        frameWholeScene(*viewport);
        REQUIRE(renderUntil(*viewport, [](const QImage &image) {
            return countWhere(image, nearlyWhite) > 100 &&
                   countWhere(image, nearlyBlue) > 1000;
        }));

        // Depth rounding changes continuously as the camera moves. Holding
        // the assertion across a sequence catches the shimmer that a single
        // top-down frame misses.
        for (int frame = 0; frame < 12; ++frame) {
            pci::testAccess(*viewport).orbitCameraForTesting(
                frame % 2 == 0 ? 1.5 : -0.75, 0.35);
            const QImage image = renderFrames(*viewport, 2);
            CAPTURE(edl, frame);
            CHECK(countWhere(image, nearlyWhite) > 50);
        }
    }
}

TEST_CASE("GPU mixed scenes preserve point raster vector painter order",
          "[gpu][raster][vector][order]")
{
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addLayer(coplanarPointGrid()));
    static_cast<void>(document->addRasterLayer(
        patternLayer({256}, [](std::uint32_t, double, double) {
            return Rgba{0, 0, 200};
        })));
    const pci::SceneLayerId vectorId = document->addVectorLayer(
        filledVectorRectangle(-40.0F, -40.0F, 40.0F, 40.0F));
    REQUIRE(document->setVectorLayerStyle(vectorId, greenVectorFill()));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setPointSizePixels(5);

    for (const bool edl : {false, true}) {
        viewport->setEyeDomeLightingEnabled(edl);
        frameWholeScene(*viewport);
        CAPTURE(edl);
        REQUIRE(renderUntil(*viewport, [](const QImage &image) {
            const Footprint footprint = renderedFootprint(image);
            return footprint.valid() &&
                   nearlyGreen(sampleWithin(image, footprint, 0.5, 0.5)) &&
                   countWhere(image, nearlyGreen) > 1000 &&
                   countWhere(image, nearlyBlue) > 1000 &&
                   countWhere(image, nearlyWhite) > 50;
        }));
    }
}

TEST_CASE("GPU raster pixels are excluded from picking and measurement",
          "[gpu][raster][picking][measurement]")
{
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(
        patternLayer({256}, [](std::uint32_t, double, double) {
            return Rgba{0, 0, 255};
        })));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);
    QString failure;
    viewport->setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    REQUIRE(renderUntil(*viewport, [](const QImage &image) {
        return countWhere(image, nearlyBlue) > 1000;
    }));

    bool pickCompleted = false;
    std::optional<std::uint32_t> picked;
    const QPoint center(viewport->width() / 2, viewport->height() / 2);
    pci::testAccess(*viewport).requestRawPickForTesting(
        center, [&](const std::optional<std::uint32_t> id) {
            pickCompleted = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pickCompleted || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    CHECK_FALSE(picked.has_value());

    viewport->setActiveTool(pci::ViewportTool::Measure);
    for (const QPoint position :
         {center, QPoint(viewport->width() / 3, viewport->height() / 3)}) {
        const std::uint64_t previous =
            pci::testAccess(*viewport).renderedFrameCountForTesting();
        QTest::mouseClick(
            viewport.get(), Qt::LeftButton, Qt::NoModifier, position);
        REQUIRE(QTest::qWaitFor(
            [&] {
                return !failure.isEmpty() ||
                       pci::testAccess(*viewport)
                               .renderedFrameCountForTesting() > previous;
            },
            5000));
        QTest::qWait(50);
    }
    REQUIRE(failure.isEmpty());
    CHECK_FALSE(pci::testAccess(*viewport).measurementHasAnchorForTesting());
    CHECK_FALSE(pci::testAccess(*viewport).measurementForTesting());
}

TEST_CASE("GPU raster textures stay inside the configured budget",
          "[gpu][raster][budget]")
{
    std::shared_ptr<PatternSource> source;
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addRasterLayer(patternLayer(
        {4096, 1024, 256},
        [](const std::uint32_t levelIndex, double, double) -> Rgba {
            return levelIndex == 0 ? Rgba{255, 0, 0} : Rgba{0, 0, 255};
        },
        &source)));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    frameWholeScene(*viewport);

    // A budget far below what a full refinement wants forces eviction rather
    // than growth.
    constexpr std::uint64_t cpuBudget = 32ULL * 1024 * 1024;
    constexpr std::uint64_t gpuBudget = 8ULL * 1024 * 1024;
    viewport->setRasterByteBudgets(cpuBudget, gpuBudget);

    for (int pass = 0; pass < 6; ++pass) {
        pci::testAccess(*viewport).frameTopDownForTesting(pass % 2 == 0 ? 0.05
                                                                        : 2.0);
        static_cast<void>(renderFrames(*viewport, 3));
        CAPTURE(pass);
        CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() <=
              gpuBudget);
        CHECK(pci::testAccess(*viewport).rasterCpuBytesForTesting() <=
              cpuBudget);
    }

    pci::testAccess(*viewport).frameTopDownForTesting(0.05);
    static_cast<void>(renderFrames(*viewport, 3));
    const std::uint64_t beforeShrink =
        pci::testAccess(*viewport).rasterGpuBytesForTesting();
    REQUIRE(beforeShrink > 1);
    const std::uint64_t loweredBudget = beforeShrink - 1;
    viewport->setRasterByteBudgets(cpuBudget, loweredBudget);
    CHECK(pci::testAccess(*viewport).rasterGpuBytesForTesting() <=
          loweredBudget);
}
