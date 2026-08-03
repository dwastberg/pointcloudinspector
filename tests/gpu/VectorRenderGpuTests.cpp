#include "app/ApplicationOptions.h"
#include "renderer/rhi/RenderViewportWidget_p.h"
#include "support/RenderViewportTestAccess.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QTest>

#include <algorithm>
#include <array>
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

std::uint64_t redPixelCount(const QImage &image)
{
    std::uint64_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 100 && color.red() > color.green() * 3 / 2 &&
                color.red() > color.blue() * 3 / 2) {
                ++count;
            }
        }
    }
    return count;
}

std::uint64_t visiblePixelCount(const QImage &image)
{
    std::uint64_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 40 || color.green() > 40 || color.blue() > 40) {
                ++count;
            }
        }
    }
    return count;
}

std::uint64_t
redPixelCount(const QImage &image, const int minimumX, const int maximumX)
{
    std::uint64_t count = 0;
    const int from = std::clamp(minimumX, 0, image.width());
    const int to = std::clamp(maximumX, from, image.width());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = from; x < to; ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 100 && color.red() > color.green() * 3 / 2 &&
                color.red() > color.blue() * 3 / 2) {
                ++count;
            }
        }
    }
    return count;
}

QColor centerPixel(const QImage &image)
{
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

bool redDominant(const QColor &color)
{
    return color.red() > 160 && color.red() > color.green() * 2 &&
           color.red() > color.blue() * 2;
}

bool greenDominant(const QColor &color)
{
    return color.green() > 160 && color.green() > color.red() * 2 &&
           color.green() > color.blue() * 2;
}

QImage grabRenderedFrame(pci::RenderViewportWidget &viewport,
                         const bool expectEyeDomeLighting)
{
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                       0 &&
                   (!expectEyeDomeLighting ||
                    pci::testAccess(viewport)
                        .eyeDomeLightingActiveForTesting());
        },
        2000));
    const std::uint64_t previous =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.requestRender();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   previous;
        },
        2000));
    const QImage image = viewport.grabFramebuffer();
    REQUIRE_FALSE(image.isNull());
    return image;
}

pci::PointCloudScenePtr pointSurfaceScene()
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 21U * 21U;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    metadata.hasColor = true;
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {-1.0, -1.0, -1.0};
    block->scale = 2.0 / 65535.0;
    // Keep framing deterministic and independent of the deliberately compact
    // splat surface.
    block->bounds = metadata.sourceBounds;
    for (int y = -10; y <= 10; ++y) {
        for (int x = -10; x <= 10; ++x) {
            const pci::Vec3d position{
                static_cast<double>(x) * 0.04,
                static_cast<double>(y) * 0.04,
                0.0,
            };
            const auto q =
                pci::quantizeToBlock(position, block->origin, block->scale);
            block->points.push_back({
                .x = q[0],
                .y = q[1],
                .z = q[2],
                .attributes = 0,
                .rgba = 0xffffffffU,
                .packedProperties = 0,
            });
            block->attributes.emplace_back();
        }
    }
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

pci::SceneDocumentPtr
pointSurfaceDocument(pci::PointCloudLayerId *layerId = nullptr)
{
    auto document = std::make_shared<pci::SceneDocument>();
    const pci::PointCloudLayerId added =
        document->addLayer(pointSurfaceScene());
    if (layerId) {
        *layerId = added;
    }
    return document;
}

pci::VectorLayerDataPtr filledRectangle(const float minimumX,
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

pci::VectorLayerDataPtr lineData(const pci::VectorSegment2f segment)
{
    auto data = std::make_shared<pci::VectorLayerData>();
    data->origin = {};
    data->segments.push_back(segment);
    data->bounds = {
        .minimum =
            {
                std::min(segment.x0, segment.x1),
                std::min(segment.y0, segment.y1),
                0.0,
            },
        .maximum =
            {
                std::max(segment.x0, segment.x1),
                std::max(segment.y0, segment.y1),
                0.0,
            },
    };
    data->summary.lineParts = 1;
    data->featureCount = 1;
    return data;
}

pci::VectorLayerStyle opaqueFill(const pci::VectorRgba color,
                                 const double zOffset,
                                 const bool alwaysOnTop)
{
    pci::VectorLayerStyle style;
    style.fill = color;
    style.stroke.alpha = 0.0F;
    style.marker.alpha = 0.0F;
    style.opacity = 1.0F;
    style.zOffset = zOffset;
    style.alwaysOnTop = alwaysOnTop;
    return style;
}

pci::VectorLayerStyle opaqueLine()
{
    pci::VectorLayerStyle style;
    style.fill.alpha = 0.0F;
    style.stroke = {1.0F, 0.0F, 0.0F, 1.0F};
    style.marker.alpha = 0.0F;
    style.strokeWidthPixels = 10.0F;
    style.opacity = 1.0F;
    return style;
}

enum class DepthScenario {
    Behind,
    InFront,
    AlwaysOnTop,
    Background,
};

const char *depthScenarioName(const DepthScenario scenario)
{
    switch (scenario) {
    case DepthScenario::Behind:
        return "behind";
    case DepthScenario::InFront:
        return "in-front";
    case DepthScenario::AlwaysOnTop:
        return "always-on-top";
    case DepthScenario::Background:
        return "background";
    }
    return "unknown";
}

enum class NearPlaneScenario {
    BetweenEyeAndNear,
    BehindEye,
    BothBehindNear,
    Grazing,
};

const char *nearPlaneScenarioName(const NearPlaneScenario scenario)
{
    switch (scenario) {
    case NearPlaneScenario::BetweenEyeAndNear:
        return "between-eye-and-near";
    case NearPlaneScenario::BehindEye:
        return "behind-eye";
    case NearPlaneScenario::BothBehindNear:
        return "both-behind-near";
    case NearPlaneScenario::Grazing:
        return "grazing";
    }
    return "unknown";
}

} // namespace

TEST_CASE("GPU renders a vector-only marker with EDL on and off",
          "[gpu][vector][smoke]")
{
    auto data = std::make_shared<pci::VectorLayerData>();
    data->origin = {};
    data->markers.push_back({0.0F, 0.0F});
    data->bounds = {
        .minimum = {-0.1, -0.1, 0.0},
        .maximum = {0.1, 0.1, 0.0},
    };
    data->summary.pointParts = 1;
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addVectorLayer(std::move(data)));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setDocument(document->snapshot(), true);
    CHECK(visiblePixelCount(grabRenderedFrame(viewport, true)) > 0);

    const std::uint64_t frame =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.setEyeDomeLightingEnabled(false);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   frame;
        },
        2000));
    const QImage plain = viewport.grabFramebuffer();
    REQUIRE_FALSE(plain.isNull());
    CHECK(visiblePixelCount(plain) > 0);
}

TEST_CASE("GPU vector depth matrix is stable with EDL on and off",
          "[gpu][vector][depth][edl]")
{
    const bool eyeDomeLighting = GENERATE(false, true);
    const DepthScenario scenario = GENERATE(DepthScenario::Behind,
                                            DepthScenario::InFront,
                                            DepthScenario::AlwaysOnTop,
                                            DepthScenario::Background);
    CAPTURE(eyeDomeLighting, depthScenarioName(scenario));

    auto document = pointSurfaceDocument();
    const bool background = scenario == DepthScenario::Background;
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setPointSizePixels(pci::maximumPointSizePixels);
    viewport.setEyeDomeLightingEnabled(eyeDomeLighting);
    viewport.setDocument(document->snapshot(), true);
    viewport.frameVisibleLayersTopDown();

    const pci::SceneLayerId vectorId = document->addVectorLayer(
        background ? filledRectangle(2.0F, -0.30F, 3.0F, 0.30F)
                   : filledRectangle(-0.28F, -0.28F, 0.28F, 0.28F));
    const bool alwaysOnTop = scenario == DepthScenario::AlwaysOnTop;
    double zOffset = scenario == DepthScenario::InFront ? 0.25 : -0.25;
    if (background) {
        const pci::NavigationCamera &camera =
            pci::testAccess(viewport).cameraForTesting();
        const double depth = camera.clipPlanes().farPlane * 0.98;
        zOffset = camera.position().z + camera.forward().z * depth;
    }
    REQUIRE(document->setVectorLayerStyle(
        vectorId, opaqueFill({1.0F, 0.0F, 0.0F, 1.0F}, zOffset, alwaysOnTop)));
    viewport.updateDocument(document->snapshot());
    const QImage image = grabRenderedFrame(viewport, eyeDomeLighting);

    if (background) {
        // This is also the final-target background-depth check: when EDL is
        // active its fullscreen composite must republish 1.0 at pixels with no
        // points, otherwise this tested vector is rejected by stale depth.
        CHECK(redPixelCount(image) > 40);
    } else if (scenario == DepthScenario::Behind) {
        CHECK_FALSE(redDominant(centerPixel(image)));
    } else {
        CHECK(redDominant(centerPixel(image)));
    }
}

TEST_CASE("GPU vector near-plane clipping remains finite with EDL on and off",
          "[gpu][vector][near-plane][edl]")
{
    const bool eyeDomeLighting = GENERATE(false, true);
    const NearPlaneScenario scenario =
        GENERATE(NearPlaneScenario::BetweenEyeAndNear,
                 NearPlaneScenario::BehindEye,
                 NearPlaneScenario::BothBehindNear,
                 NearPlaneScenario::Grazing);
    CAPTURE(eyeDomeLighting, nearPlaneScenarioName(scenario));

    pci::PointCloudLayerId framingLayer;
    auto document = pointSurfaceDocument(&framingLayer);
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setEyeDomeLightingEnabled(eyeDomeLighting);
    viewport.setDocument(document->snapshot(), true);

    const pci::Vec3d eye =
        pci::testAccess(viewport).cameraForTesting().position();
    const double nearPlane =
        pci::testAccess(viewport).cameraForTesting().clipPlanes().nearPlane;
    pci::VectorSegment2f segment;
    switch (scenario) {
    case NearPlaneScenario::BetweenEyeAndNear:
        segment = {
            -static_cast<float>(nearPlane * 0.3),
            static_cast<float>(eye.y + nearPlane * 0.5),
            0.40F,
            0.0F,
        };
        break;
    case NearPlaneScenario::BehindEye:
        segment = {
            -0.05F,
            static_cast<float>(eye.y - 0.20),
            0.40F,
            0.0F,
        };
        break;
    case NearPlaneScenario::BothBehindNear:
        segment = {
            -0.05F,
            static_cast<float>(eye.y - 0.20),
            0.05F,
            static_cast<float>(eye.y + nearPlane * 0.5),
        };
        break;
    case NearPlaneScenario::Grazing:
        segment = {
            -static_cast<float>(nearPlane * 0.5),
            static_cast<float>(eye.y + nearPlane * 1.01),
            0.50F,
            static_cast<float>(eye.y + 0.30),
        };
        break;
    }
    REQUIRE(document->setLayerVisible(framingLayer, false));
    const pci::SceneLayerId vectorId =
        document->addVectorLayer(lineData(segment));
    REQUIRE(document->setVectorLayerStyle(vectorId, opaqueLine()));
    viewport.updateDocument(document->snapshot());

    const QImage image = grabRenderedFrame(viewport, eyeDomeLighting);
    const std::uint64_t red = redPixelCount(image);
    INFO("red pixels: " << red);
    if (scenario == NearPlaneScenario::BothBehindNear) {
        CHECK(red == 0);
    } else {
        CHECK(red > 10);
        CHECK(red <
              static_cast<std::uint64_t>(image.width() * image.height() / 8));
    }
}

TEST_CASE("GPU vector polygon fill readback matches the configured color",
          "[gpu][vector][fill]")
{
    pci::PointCloudLayerId framingLayer;
    auto document = pointSurfaceDocument(&framingLayer);
    REQUIRE(document->setLayerVisible(framingLayer, false));
    const pci::SceneLayerId vectorId =
        document->addVectorLayer(filledRectangle(-0.45F, -0.45F, 0.45F, 0.45F));
    REQUIRE(document->setVectorLayerStyle(
        vectorId, opaqueFill({1.0F, 0.0F, 0.0F, 1.0F}, 0.0, false)));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setEyeDomeLightingEnabled(false);
    viewport.setDocument(document->snapshot(), true);
    viewport.frameVisibleLayersTopDown();
    const QColor center = centerPixel(grabRenderedFrame(viewport, false));
    CHECK(center.red() >= 245);
    CHECK(center.green() <= 10);
    CHECK(center.blue() <= 10);
}

TEST_CASE("GPU renders a connected fill across two 16-bit batches",
          "[gpu][vector][fill][batching]")
{
    pci::PointCloudLayerId framingLayer;
    auto document = pointSurfaceDocument(&framingLayer);
    REQUIRE(document->setLayerVisible(framingLayer, false));

    auto data = std::make_shared<pci::VectorLayerData>();
    data->origin = {};
    pci::VectorFillBatch left{
        .vertices =
            {
                {-0.8F, -0.4F},
                {0.0F, -0.4F},
                {0.0F, 0.4F},
                {-0.8F, 0.4F},
            },
        .indices = {0, 1, 2, 0, 2, 3},
    };
    // Exercise the universal 16-bit policy at its exact local-vertex ceiling.
    // The duplicated seam in the second batch represents a connected source
    // polygon partitioned at a triangle boundary.
    left.vertices.resize(pci::maximumVerticesPerFillBatch,
                         left.vertices.front());
    pci::VectorFillBatch right{
        .vertices =
            {
                {0.0F, -0.4F},
                {0.8F, -0.4F},
                {0.8F, 0.4F},
                {0.0F, 0.4F},
            },
        .indices = {0, 1, 2, 0, 2, 3},
    };
    data->fillBatches.push_back(std::move(left));
    data->fillBatches.push_back(std::move(right));
    data->bounds = {
        .minimum = {-0.8, -0.4, 0.0},
        .maximum = {0.8, 0.4, 0.0},
    };
    data->summary.polygonParts = 1;
    const pci::SceneLayerId vectorId =
        document->addVectorLayer(std::move(data));
    REQUIRE(document->setVectorLayerStyle(
        vectorId, opaqueFill({1.0F, 0.0F, 0.0F, 1.0F}, 0.0, false)));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setEyeDomeLightingEnabled(false);
    viewport.setDocument(document->snapshot(), true);
    viewport.frameVisibleLayersTopDown();
    const QImage image = grabRenderedFrame(viewport, false);
    CHECK(redPixelCount(image, 0, image.width() / 2) > 100);
    CHECK(redPixelCount(image, image.width() / 2, image.width()) > 100);
}

TEST_CASE("GPU always-on-top overlap follows document painter order",
          "[gpu][vector][painter-order]")
{
    pci::PointCloudLayerId framingLayer;
    auto document = pointSurfaceDocument(&framingLayer);
    REQUIRE(document->setLayerVisible(framingLayer, false));
    const pci::SceneLayerId first =
        document->addVectorLayer(filledRectangle(-0.45F, -0.45F, 0.45F, 0.45F));
    const pci::SceneLayerId second =
        document->addVectorLayer(filledRectangle(-0.30F, -0.30F, 0.30F, 0.30F));
    REQUIRE(document->setVectorLayerStyle(
        first, opaqueFill({1.0F, 0.0F, 0.0F, 1.0F}, 0.0, true)));
    REQUIRE(document->setVectorLayerStyle(
        second, opaqueFill({0.0F, 1.0F, 0.0F, 1.0F}, 0.0, true)));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(viewportSize, viewportSize);
    viewport.setEyeDomeLightingEnabled(true);
    viewport.setDocument(document->snapshot(), true);
    viewport.frameVisibleLayersTopDown();
    const QColor center = centerPixel(grabRenderedFrame(viewport, true));
    CHECK(greenDominant(center));
}
