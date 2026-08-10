#include "app/ApplicationOptions.h"
#include "renderer/rhi/RenderViewportWidget_p.h"
#include "scene/PointCloudScene.h"
#include "support/RenderViewportTestAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
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

class PatternSource final : public pci::RasterTileSource {
public:
    PatternSource(pci::RasterLayerMetadata metadata, LevelPainter painter)
        : metadata_(std::move(metadata))
        , painter_(std::move(painter))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData
    readTile(const pci::RasterTileRequest &request,
             std::stop_token stop) const override
    {
        if (stop.stop_requested()) {
            throw pci::RasterReadCancelled{};
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
        tile.validWidth = static_cast<std::uint16_t>(extent.width);
        tile.validHeight = static_cast<std::uint16_t>(extent.height);
        tile.rgba.assign(pci::rasterStoredTileBytes, std::byte{0});
        ++reads;

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
            }
        }
        return tile;
    }

    mutable std::atomic_int reads{0};

private:
    pci::RasterLayerMetadata metadata_;
    LevelPainter painter_;
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

[[nodiscard]] pci::RasterLayerDataPtr
patternLayer(const std::vector<std::uint32_t> &levelWidths,
             LevelPainter painter,
             std::shared_ptr<PatternSource> *sourceOut = nullptr)
{
    auto source = std::make_shared<PatternSource>(patternMetadata(levelWidths),
                                                  std::move(painter));
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

[[nodiscard]] std::unique_ptr<pci::RenderViewportWidget> makeViewport()
{
    auto viewport = std::make_unique<pci::RenderViewportWidget>(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport->resize(viewportSize, viewportSize);
    return viewport;
}

} // namespace

TEST_CASE("GPU raster pixels land at their affine georeferenced coordinates",
          "[gpu][raster][placement]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 256;
    metadata.height = 256;
    // A rotated and skewed parallelogram. Its four corners are deliberately
    // asymmetric so a center-only or north-up placement can not pass.
    metadata.geoTransform = {
        -80.0, 200.0 / 256.0, 40.0 / 256.0,
        60.0, 50.0 / 256.0, -160.0 / 256.0,
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
        [width = metadata.width,
         height = metadata.height](std::uint32_t,
                                   const double pixel,
                                   const double line) -> Rgba {
            if (pixel < width * 0.5 && line < height * 0.5) {
                return {255, 0, 0};
            }
            if (pixel >= width * 0.5 && line < height * 0.5) {
                return {0, 255, 0};
            }
            return pixel < width * 0.5 ? Rgba{0, 0, 255}
                                       : Rgba{255, 255, 0};
        })));

    auto viewport = makeViewport();
    viewport->setDocument(document->snapshot(), true);
    viewport->setEyeDomeLightingEnabled(false);
    viewport->setOrthographic(true);
    frameWholeScene(*viewport);

    const auto screenAtBasePixel = [&metadata, &viewport](
                                       const QSize imageSize,
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
    const auto sampleAtBasePixel = [&screenAtBasePixel](
                                       const QImage &image,
                                       const double pixel,
                                       const double line) {
        const QPoint position =
            screenAtBasePixel(image.size(), pixel, line);
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

TEST_CASE("GPU coplanar points remain visible over rasters through an EDL camera sweep",
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
}
