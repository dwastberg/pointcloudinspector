#include "support/RenderViewportTestAccess.h"
#include "support/SceneRuntimeFixture.h"
#include <pci/desktop/config/ApplicationOptions.h>
#include <pci/desktop/viewport/RenderViewportWidget_p.h>
#include <pci/document/SceneDocument.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QImage>
#include <QTest>
#include <QWheelEvent>
#include <rhi/qrhi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

pci::GraphicsApi gpuTestGraphicsApi()
{
    const auto api = pci::parseGraphicsApi(PCINSPECTOR_GPU_TEST_GRAPHICS_API);
    if (!api) {
        throw std::logic_error("invalid configured GPU test graphics API");
    }
    return *api;
}

constexpr bool gpuTestValidation = PCINSPECTOR_GPU_TEST_VALIDATION != 0;

pci::PointDatasetRuntimePtr
sceneWithBlocksAt(const std::vector<pci::Vec3d> &positions,
                  const bool hasColor = false)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = positions.size();
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    metadata.hasColor = hasColor;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    for (const pci::Vec3d &position : positions) {
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / 65535.0;
        block->bounds = {
            .minimum = {position.x - 0.01,
                        position.y - 0.01,
                        position.z - 0.01},
            .maximum = {position.x + 0.01,
                        position.y + 0.01,
                        position.z + 0.01},
        };
        const auto q =
            pci::quantizeToBlock(position, block->origin, block->scale);
        block->points.push_back({.x = q[0],
                                 .y = q[1],
                                 .z = q[2],
                                 .attributes = 0,
                                 .rgba = 0xffffffffU,
                                 .packedProperties = 0});
        block->attributes.resize(1);
        scene->addBlock(std::move(block));
    }
    scene->markLoadingComplete();
    return scene;
}

// A completed flat scene whose bounds sit entirely at `center`. Used to place a
// source far outside the framed extent, the way adjacent survey tiles spread
// across kilometres while the camera frames only the first one loaded.
pci::PointDatasetRuntimePtr completedSceneAtCenter(const pci::Vec3d center)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {
        .minimum = {center.x - 1.0, center.y - 1.0, center.z - 1.0},
        .maximum = {center.x + 1.0, center.y + 1.0, center.z + 1.0},
    };
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {center.x - 1.0, center.y - 1.0, center.z - 1.0};
    block->scale = 2.0 / 65535.0;
    block->bounds = {
        .minimum = {center.x - 0.01, center.y - 0.01, center.z - 0.01},
        .maximum = {center.x + 0.01, center.y + 0.01, center.z + 0.01},
    };
    const auto quantized =
        pci::quantizeToBlock(center, block->origin, block->scale);
    block->points.push_back({.x = quantized[0],
                             .y = quantized[1],
                             .z = quantized[2],
                             .attributes = 0,
                             .rgba = 0xffffffffU,
                             .packedProperties = 0});
    block->attributes.resize(1);
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

pci::PointDatasetRuntimePtr coincidentPointSizeScene()
{
    // Enough coincident samples to keep the minimum bias at the 1 px clamp
    // while the maximum bias remains large enough to exercise largePoints.
    constexpr std::size_t pointCount = 262'144;
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = pointCount;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    metadata.hasColor = true;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {-1.0, -1.0, -1.0};
    block->scale = 2.0 / 65535.0;
    block->bounds = metadata.sourceBounds;
    const auto q =
        pci::quantizeToBlock({0.0, 0.0, 0.0}, block->origin, block->scale);
    block->points.resize(pointCount,
                         {.x = q[0],
                          .y = q[1],
                          .z = q[2],
                          .attributes = 0,
                          .rgba = 0xffffffffU,
                          .packedProperties = 0});
    block->attributes.resize(pointCount);
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

pci::test::SceneRuntimeFixture
documentWithScene(const pci::PointDatasetRuntimePtr &scene)
{
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    static_cast<void>(runtime.addPointLayer(scene));
    return runtime;
}

pci::test::SceneRuntimeFixture
documentWithScenes(std::vector<pci::PointDatasetRuntimePtr> scenes)
{
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    for (const pci::PointDatasetRuntimePtr &scene : scenes) {
        static_cast<void>(runtime.addPointLayer(scene));
    }
    return runtime;
}

class GpuStressHierarchySource final : public pci::PointCloudDataSource {
public:
    static constexpr std::size_t pointsPerNode = 64;
    static constexpr std::uint64_t leafPointCount = 8 * pointsPerNode;

    explicit GpuStressHierarchySource(const std::uint32_t color)
        : color_(color)
    {
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        const std::uint64_t cells = std::uint64_t{1} << id.level;
        if (id.level > 1 || id.x >= cells || id.y >= cells || id.z >= cells) {
            throw std::out_of_range("GPU stress hierarchy node is invalid");
        }
        return {
            .id = id,
            .bounds = pci::pointCloudNodeBounds(bounds_, id),
            .geometricError = id.level == 0 ? 4.0 : 0.0,
            .estimatedPointCount = pointsPerNode,
            .leaf = id.level == 1,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId id,
             const std::stop_token stopToken) const override
    {
        ++requests_;
        if (stopToken.stop_requested()) {
            ++cancelled_;
            throw pci::PointCloudDataSourceCancelled();
        }
        pci::PointCloudNodePayloadPtr result = makePayload(id);
        decodedBytes_.fetch_add(pci::pointCloudNodePayloadBytes(*result),
                                std::memory_order_relaxed);
        ++completed_;
        return result;
    }

    [[nodiscard]] pci::PointCloudDataSourceMetrics metrics() const override
    {
        const std::uint64_t requests =
            requests_.load(std::memory_order_relaxed);
        return {
            .requests = requests,
            .completed = completed_.load(std::memory_order_relaxed),
            .cancelled = cancelled_.load(std::memory_order_relaxed),
            .estimatedDecodedBytesRequested =
                requests * pointsPerNode *
                (sizeof(pci::GpuPoint) + sizeof(pci::PointAttributes)),
            .decodedBytesProduced =
                decodedBytes_.load(std::memory_order_relaxed),
            .decodedPointsProduced =
                completed_.load(std::memory_order_relaxed) * pointsPerNode,
        };
    }

    [[nodiscard]] std::optional<pci::PointCloudFullDetailInfo>
    fullDetailInfo() const override
    {
        const auto children = pci::childNodeIds(pci::rootPointCloudNode);
        return pci::PointCloudFullDetailInfo{
            .leafNodes = std::vector<pci::PointCloudNodeId>(children.begin(),
                                                            children.end()),
            .pointCount = leafPointCount,
            .decodedBytes = leafPointCount * (sizeof(pci::GpuPoint) +
                                              sizeof(pci::PointAttributes)),
            .gpuBytes = leafPointCount * sizeof(pci::GpuPoint),
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr rootPayload() const
    {
        return makePayload(pci::rootPointCloudNode);
    }

private:
    [[nodiscard]] pci::PointCloudNodePayloadPtr
    makePayload(const pci::PointCloudNodeId id) const
    {
        const pci::Bounds3d nodeBounds = node(id).bounds;
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {
            nodeBounds.minimum[0],
            nodeBounds.minimum[1],
            nodeBounds.minimum[2],
        };
        block->scale = nodeBounds.maximumExtent() / 65535.0;
        block->bounds = nodeBounds;
        const auto center = nodeBounds.center();
        const auto quantized = pci::quantizeToBlock(
            {center[0], center[1], center[2]}, block->origin, block->scale);
        block->points.resize(pointsPerNode,
                             {
                                 .x = quantized[0],
                                 .y = quantized[1],
                                 .z = quantized[2],
                                 .rgba = color_,
                             });
        block->attributes.resize(pointsPerNode);

        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->sourcePointCount = pointsPerNode;
        result->blocks.push_back(std::move(block));
        return result;
    }

    const std::uint32_t color_;
    const pci::Bounds3d bounds_{
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    mutable std::atomic_uint64_t requests_ = 0;
    mutable std::atomic_uint64_t completed_ = 0;
    mutable std::atomic_uint64_t cancelled_ = 0;
    mutable std::atomic_uint64_t decodedBytes_ = 0;
};

struct GpuStressHierarchy {
    std::shared_ptr<GpuStressHierarchySource> source;
    pci::PointCloudNodePayloadPtr root;
    pci::PointDatasetRuntimePtr scene;
};

GpuStressHierarchy
makeGpuStressHierarchy(const std::uint32_t color,
                       const std::uint64_t reportedSourcePointCount =
                           GpuStressHierarchySource::leafPointCount)
{
    auto source = std::make_shared<GpuStressHierarchySource>(color);
    pci::PointCloudNodePayloadPtr root = source->rootPayload();
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = reportedSourcePointCount;
    metadata.sourceBounds = source->rootNode().bounds;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(
        metadata, source, root, pci::defaultDecodedCacheByteBudget);
    scene->markLoadingComplete();
    return {
        .source = std::move(source),
        .root = std::move(root),
        .scene = std::move(scene),
    };
}

pci::PointDatasetRuntimePtr
sceneWithRepeatedBlocksAt(const pci::Vec3d position,
                          const std::size_t blockCount)
{
    return sceneWithBlocksAt(std::vector<pci::Vec3d>(blockCount, position));
}

pci::PointDatasetRuntimePtr completedDenseScene(const std::uint64_t pointCount)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = pointCount;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    std::uint64_t remaining = pointCount;
    while (remaining > 0) {
        const std::size_t blockPoints = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, pci::maximumPointsPerBlock));
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / 65535.0;
        block->bounds = {
            .minimum = {-0.01, -0.01, -0.01},
            .maximum = {0.01, 0.01, 0.01},
        };
        const auto quantized =
            pci::quantizeToBlock({0.0, 0.0, 0.0}, block->origin, block->scale);
        block->points.resize(blockPoints,
                             {
                                 .x = quantized[0],
                                 .y = quantized[1],
                                 .z = quantized[2],
                                 .rgba = 0xffffffffU,
                             });
        block->attributes.resize(blockPoints);
        scene->addBlock(std::move(block));
        remaining -= blockPoints;
    }
    scene->markLoadingComplete();
    return scene;
}

pci::PointDatasetRuntimePtr eyeDomeDepthStepScene()
{
    constexpr int pointColumns = 41;
    constexpr int pointRows = 41;
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = pointColumns * pointRows;
    metadata.sourceBounds = {
        .minimum = {-0.06, -0.5, -0.06},
        .maximum = {0.06, 0.5, 0.06},
    };
    metadata.hasColor = true;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {-0.06, -0.5, -0.06};
    block->scale = 1.0 / 65535.0;
    block->bounds = metadata.sourceBounds;
    block->points.reserve(pointColumns * pointRows);
    for (int row = -20; row <= 20; ++row) {
        for (int column = -20; column <= 20; ++column) {
            const pci::Vec3d position{
                static_cast<double>(column) * 0.003,
                column < 0 ? -0.5 : 0.5,
                static_cast<double>(row) * 0.003,
            };
            const auto quantized =
                pci::quantizeToBlock(position, block->origin, block->scale);
            block->points.push_back({
                .x = quantized[0],
                .y = quantized[1],
                .z = quantized[2],
                .rgba = 0xffffffffU,
            });
        }
    }
    block->attributes.resize(block->points.size());
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();
    return scene;
}

bool waitForStableFrameCount(pci::RenderViewportWidget &viewport)
{
    std::uint64_t previous =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    int stableSamples = 0;
    for (int sample = 0; sample < 20; ++sample) {
        QTest::qWait(25);
        const std::uint64_t current =
            pci::testAccess(viewport).renderedFrameCountForTesting();
        if (current == previous) {
            if (++stableSamples == 3) {
                return true;
            }
        } else {
            stableSamples = 0;
            previous = current;
        }
    }
    return false;
}

TEST_CASE("GPU qualification camera path advances on submitted frames",
          "[gpu][qualification][camera-path]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(640, 400);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        true);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() > 0;
        },
        5000));
    REQUIRE(waitForStableFrameCount(viewport));

    std::vector<pci::RenderMetrics> frames;
    viewport.setMetricsCallback([&frames](const pci::RenderMetrics &metrics) {
        if (metrics.qualificationFrame) {
            frames.push_back(metrics);
        }
    });
    viewport.setContinuousMetricsEnabled(true);
    REQUIRE(viewport.startQualificationCameraPath());
    const bool replayCompleted = QTest::qWaitFor(
        [&] {
            return !frames.empty() && frames.back().qualificationFinalFrame;
        },
        20'000);
    INFO("qualification samples: " << frames.size());
    INFO("submitted frames: "
         << pci::testAccess(viewport).renderedFrameCountForTesting());
    if (!frames.empty()) {
        INFO("last qualification frame: "
             << frames.back().qualificationFrameIndex);
    }
    REQUIRE(replayCompleted);
    viewport.setContinuousMetricsEnabled(false);

    REQUIRE(frames.size() == 360);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        CHECK(frames[index].qualificationFrameIndex == index);
        CHECK(frames[index].qualificationFrameCount == frames.size());
    }
    CHECK(std::ranges::count(frames,
                             QStringLiteral("warmup"),
                             &pci::RenderMetrics::qualificationPhase) == 30);
    CHECK(std::ranges::count(frames,
                             QStringLiteral("interacting"),
                             &pci::RenderMetrics::qualificationPhase) == 210);
    CHECK(std::ranges::count(frames,
                             QStringLiteral("dwell"),
                             &pci::RenderMetrics::qualificationPhase) == 120);
}

std::uint64_t brightPixelCount(const QImage &image)
{
    std::uint64_t result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) + qGreen(pixel) + qBlue(pixel) > 600) {
                ++result;
            }
        }
    }
    return result;
}

TEST_CASE("GPU eye-dome lighting toggles and rebuilds safely", "[gpu][edl]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport, documentWithScene(eyeDomeDepthStepScene()), true);
    viewport.show();

    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                       0 &&
                   pci::testAccess(viewport).eyeDomeLightingActiveForTesting();
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const QImage eyeDomeImage = viewport.grabFramebuffer();
    REQUIRE_FALSE(eyeDomeImage.isNull());

    std::uint64_t frame =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.setEyeDomeLightingEnabled(false);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                       frame &&
                   !pci::testAccess(viewport).eyeDomeLightingActiveForTesting();
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const QImage plainImage = viewport.grabFramebuffer();
    REQUIRE_FALSE(plainImage.isNull());
    REQUIRE(plainImage.size() == eyeDomeImage.size());
    std::uint64_t differingPixels = 0;
    std::uint64_t eyeDomeLuminance = 0;
    std::uint64_t plainLuminance = 0;
    for (int y = 0; y < plainImage.height(); ++y) {
        for (int x = 0; x < plainImage.width(); ++x) {
            const QRgb eyeDomePixel = eyeDomeImage.pixel(x, y);
            const QRgb plainPixel = plainImage.pixel(x, y);
            differingPixels += eyeDomePixel != plainPixel ? 1U : 0U;
            eyeDomeLuminance +=
                qRed(eyeDomePixel) + qGreen(eyeDomePixel) + qBlue(eyeDomePixel);
            plainLuminance +=
                qRed(plainPixel) + qGreen(plainPixel) + qBlue(plainPixel);
        }
    }
    INFO("different pixels: " << differingPixels);
    INFO("EDL luminance: " << eyeDomeLuminance);
    INFO("plain luminance: " << plainLuminance);
    CHECK(differingPixels > 0);
    CHECK(eyeDomeLuminance < plainLuminance);

    frame = pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.setEyeDomeLightingEnabled(true);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                       frame &&
                   pci::testAccess(viewport).eyeDomeLightingActiveForTesting();
        },
        2000));
}

TEST_CASE("GPU point size changes rasterized point coverage",
          "[gpu][point-size]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    viewport.setEyeDomeLightingEnabled(false);
    viewport.setPointSizePixels(pci::minimumPointSizePixels);
    pci::test::setTestDocument(
        viewport, documentWithScene(coincidentPointSizeScene()), true);
    viewport.show();

    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() > 0;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const QImage smallImage = viewport.grabFramebuffer();
    REQUIRE_FALSE(smallImage.isNull());
    const std::uint64_t smallCoverage = brightPixelCount(smallImage);

    const std::uint64_t frame =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.setPointSizePixels(pci::maximumPointSizePixels);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   frame;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const QImage largeImage = viewport.grabFramebuffer();
    REQUIRE_FALSE(largeImage.isNull());
    REQUIRE(largeImage.size() == smallImage.size());
    const std::uint64_t largeCoverage = brightPixelCount(largeImage);

    INFO("minimum point-size bias coverage: " << smallCoverage);
    INFO("maximum point-size bias coverage: " << largeCoverage);
    CHECK(smallCoverage > 0);
    CHECK(largeCoverage >= 32);
    CHECK(largeCoverage > smallCoverage * 8);
}

TEST_CASE("GPU classification masks hide and restore matching points",
          "[gpu][classification]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    metadata.hasColor = true;
    metadata.hasClassification = true;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = {-1.0, -1.0, -1.0};
    block->scale = 2.0 / 65535.0;
    block->bounds = {
        .minimum = {-0.01, -0.01, -0.01},
        .maximum = {0.01, 0.01, 0.01},
    };
    const auto quantized =
        pci::quantizeToBlock({0.0, 0.0, 0.0}, block->origin, block->scale);
    block->points.push_back({
        .x = quantized[0],
        .y = quantized[1],
        .z = quantized[2],
        .attributes = 2,
        .rgba = 0xffffffffU,
    });
    block->attributes.push_back({.classification = 2});
    scene->addBlock(std::move(block));
    scene->markLoadingComplete();

    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    const pci::PointCloudLayerId layerId = runtime.addPointLayer(scene);
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    viewport.setEyeDomeLightingEnabled(false);
    viewport.setPointSizePixels(pci::maximumPointSizePixels);
    runtime.setDocument(viewport, true);
    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.show();

    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() > 0;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const std::uint64_t visibleCoverage =
        brightPixelCount(viewport.grabFramebuffer());
    REQUIRE(visibleCoverage > 0);

    const std::uint64_t visibleFrame =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    CHECK(document->setLayerClassificationFilter(
        layerId, pci::PointClassificationFilter::noneVisible()));
    runtime.updateDocument(viewport);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   visibleFrame;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    CHECK(brightPixelCount(viewport.grabFramebuffer()) == 0);
    bool pickCompleted = false;
    std::optional<std::uint32_t> picked;
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
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

    pci::PointClassificationFilter classTwoOnly =
        pci::PointClassificationFilter::noneVisible();
    classTwoOnly.setVisible(2, true);
    const std::uint64_t hiddenFrame =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    CHECK(document->setLayerClassificationFilter(layerId, classTwoOnly));
    runtime.updateDocument(viewport);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   hiddenFrame;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    CHECK(brightPixelCount(viewport.grabFramebuffer()) == visibleCoverage);
    pickCompleted = false;
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            pickCompleted = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pickCompleted || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(picked.has_value());
    CHECK(*picked == 0);
}

TEST_CASE("GPU viewport sleeps when idle and wakes for worker blocks", "[gpu]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 1;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto scene = std::make_shared<pci::PointDatasetRuntime>(metadata);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    static_cast<void>(runtime.addPointLayer(scene));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    runtime.setDocument(viewport, false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() > 0;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const std::uint64_t idleFrames =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics &&
                   latestMetrics->submittedFrameCount == idleFrames;
        },
        2000));
    CHECK(latestMetrics->selectedPoints == 0);
    CHECK(latestMetrics->submittedPoints == 0);
    CHECK(latestMetrics->drawCalls == 0);
    CHECK(latestMetrics->sourcePoints == 1);
    CHECK(latestMetrics->retainedFlatPoints == 0);
    CHECK(latestMetrics->decodedResidentPoints == 0);
    CHECK(latestMetrics->gpuResidentPoints == 0);
    QTest::qWait(150);
    CHECK(pci::testAccess(viewport).renderedFrameCountForTesting() ==
          idleFrames);

    std::thread publisher([scene] {
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / 65535.0;
        block->bounds = {
            .minimum = {-0.01, -0.01, -0.01},
            .maximum = {0.01, 0.01, 0.01},
        };
        const auto quantized =
            pci::quantizeToBlock({0.0, 0.0, 0.0}, block->origin, block->scale);
        block->points.push_back({
            .x = quantized[0],
            .y = quantized[1],
            .z = quantized[2],
            .rgba = 0xffffffffU,
        });
        block->attributes.resize(1);
        scene->addBlock(std::move(block));
    });
    publisher.join();

    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   idleFrames;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const std::uint64_t publishedFrames =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics &&
                   latestMetrics->submittedFrameCount == publishedFrames &&
                   latestMetrics->submittedPoints == 1;
        },
        2000));
    CHECK(latestMetrics->requestedPoints == 1);
    CHECK(latestMetrics->selectedPoints == 1);
    CHECK(latestMetrics->drawCalls == 1);
    CHECK(latestMetrics->sourcePoints == 1);
    CHECK(latestMetrics->retainedFlatPoints == 1);
    CHECK(latestMetrics->decodedResidentPoints == 1);
    CHECK(latestMetrics->gpuResidentPoints == 1);
    CHECK(latestMetrics->visibleBlocks == 1);
    CHECK(latestMetrics->culledBlocks == 0);
    CHECK(latestMetrics->uniformDrawCapacity == 256);
    CHECK(latestMetrics->uniformCapacityGrowthCount == 0);
    CHECK(latestMetrics->uniformUpdateOperations == 1);
    CHECK(latestMetrics->uploadedPointBytes == sizeof(pci::GpuPoint));
    CHECK(latestMetrics->pendingUploadBytes == 0);
    CHECK(latestMetrics->uploadOperations == 1);
    CHECK(latestMetrics->uploadResourceUpdateBatches == 1);
    CHECK(latestMetrics->sceneSnapshotMilliseconds >= 0.0);
    CHECK(latestMetrics->selectionMilliseconds >= 0.0);
    CHECK(latestMetrics->uploadMilliseconds >= 0.0);
    CHECK(latestMetrics->commandRecordingMilliseconds >= 0.0);
    QTest::qWait(150);
    CHECK(pci::testAccess(viewport).renderedFrameCountForTesting() ==
          publishedFrames);

    viewport.requestRender();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   publishedFrames;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    const std::uint64_t reusedPlanFrames =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics &&
                   latestMetrics->submittedFrameCount == reusedPlanFrames;
        },
        2000));
    CHECK(latestMetrics->framePlanReused);
    CHECK(latestMetrics->submittedPoints == 1);
}

TEST_CASE("GPU completed flat scenes settle above the bootstrap budget",
          "[gpu][flat][budget]")
{
    constexpr std::uint64_t pointCount = 1'000'001;
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    QString failure;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    pci::test::setTestDocument(
        viewport, documentWithScene(completedDenseScene(pointCount)), false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (latestMetrics &&
                    latestMetrics->requestedPoints == pointCount &&
                    latestMetrics->submittedPoints == pointCount);
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(latestMetrics);
    CHECK(latestMetrics->sourcePoints == pointCount);
    CHECK(latestMetrics->retainedFlatPoints == pointCount);
    CHECK(latestMetrics->selectedPoints == pointCount);
    CHECK(latestMetrics->gpuResidentPoints == pointCount);
    CHECK(waitForStableFrameCount(viewport));
}

TEST_CASE("GPU residency preserves coverage across 25 flat layers",
          "[gpu][planner][fairness][multi-layer]")
{
    constexpr std::size_t layerCount = 25;
    constexpr std::uint64_t gpuByteBudget = layerCount * sizeof(pci::GpuPoint);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    std::vector<pci::PointCloudLayerId> layers;
    layers.reserve(layerCount);
    for (std::size_t index = 0; index < layerCount; ++index) {
        layers.push_back(runtime.addPointLayer(sceneWithBlocksAt({
            {0.0, 0.0, 0.0},
            {0.25, 0.0, 0.0},
        })));
    }

    pci::RenderViewportWidget viewport(
        false, gpuByteBudget, gpuTestGraphicsApi(), gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    QString failure;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    runtime.setDocument(viewport, false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (latestMetrics &&
                    latestMetrics->submittedPoints == layerCount);
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(latestMetrics);
    CHECK(latestMetrics->selectedPoints == layerCount);
    CHECK(latestMetrics->gpuResidentPoints == layerCount);
    for (const pci::PointCloudLayerId layer : layers) {
        CHECK(pci::testAccess(viewport).residentLayerPointsForTesting(layer) ==
              1);
    }
    CHECK(waitForStableFrameCount(viewport));
}

TEST_CASE("GPU layers outside the frustum still reach display readiness",
          "[gpu][residency][multi-layer][progress]")
{
    // A layer the frustum excludes is deliberately given no uploads, so it can
    // never acquire GPU residency. Display readiness must not wait on residency
    // it cannot obtain, otherwise the load that owns it never finishes.
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    const pci::PointCloudLayerId nearLayer =
        runtime.addPointLayer(completedSceneAtCenter({0.0, 0.0, 0.0}));

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    QString failure;
    std::unordered_set<pci::PointCloudLayerId> displayReady;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.setLoadProgressCallback(
        [&displayReady](const pci::RenderLoadProgress &progress) {
            if (progress.stage == pci::RenderLoadStage::DisplayReady) {
                displayReady.insert(progress.layerId);
            }
        });
    // Frame the near layer only; the far layer joins afterwards and the camera
    // is intentionally left where it is.
    runtime.setDocument(viewport, true);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() || displayReady.contains(nearLayer);
        },
        5000));
    REQUIRE(failure.isEmpty());

    const pci::PointCloudLayerId farLayer = runtime.addPointLayer(
        completedSceneAtCenter({100'000.0, 100'000.0, 0.0}));
    runtime.updateDocument(viewport);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() || displayReady.contains(farLayer);
        },
        5000));
    REQUIRE(failure.isEmpty());
    CHECK(displayReady.contains(farLayer));
    // It really is off screen: readiness came from the frustum exemption, not
    // from uploads that would contradict the premise of the test.
    CHECK(pci::testAccess(viewport).residentLayerPointsForTesting(farLayer) ==
          0);
    CHECK(pci::testAccess(viewport).residentLayerPointsForTesting(nearLayer) ==
          1);
    CHECK(waitForStableFrameCount(viewport));
}

TEST_CASE("GPU replaces root buffers after document root resampling",
          "[gpu][hierarchy][residency][fairness]")
{
    GpuStressHierarchy first = makeGpuStressHierarchy(0xffff8080U);
    GpuStressHierarchy second = makeGpuStressHierarchy(0xff80ffffU);
    const std::uint64_t oneRootBytes =
        pci::pointCloudNodePayloadBytes(*first.root);
    const std::uint64_t gpuByteBudget =
        GpuStressHierarchySource::pointsPerNode * sizeof(pci::GpuPoint);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document, oneRootBytes, 1);
    const pci::PointCloudLayerId firstLayer =
        runtime.addPointLayer(first.scene);

    pci::RenderViewportWidget viewport(
        false, gpuByteBudget, gpuTestGraphicsApi(), gpuTestValidation);
    viewport.resize(320, 240);
    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    runtime.setDocument(viewport, false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   pci::testAccess(viewport).residentLayerPointsForTesting(
                       firstLayer) == GpuStressHierarchySource::pointsPerNode;
        },
        5000));
    REQUIRE(failure.isEmpty());

    const pci::PointCloudLayerId secondLayer =
        runtime.addPointLayer(second.scene);
    runtime.updateDocument(viewport);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (pci::testAccess(viewport).residentLayerPointsForTesting(
                        firstLayer) == 32 &&
                    pci::testAccess(viewport).residentLayerPointsForTesting(
                        secondLayer) == 32);
        },
        5000));
    REQUIRE(failure.isEmpty());
    CHECK(first.scene->decodedResidentPoints() >= 32);
    CHECK(second.scene->decodedResidentPoints() >= 32);
    CHECK(runtime.runtime().decodedResidentBytes() <= oneRootBytes);
    CHECK(waitForStableFrameCount(viewport));
}

TEST_CASE("GPU finite hierarchies settle through screen-space LOD",
          "[gpu][hierarchy][lod]")
{
    GpuStressHierarchy hierarchy = makeGpuStressHierarchy(0xffffffffU);
    const std::uint64_t payloadBytes =
        pci::pointCloudNodePayloadBytes(*hierarchy.root);
    const std::uint64_t decodedByteBudget = 12 * payloadBytes;
    const std::uint64_t gpuByteBudget =
        12 * GpuStressHierarchySource::pointsPerNode * sizeof(pci::GpuPoint);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document, decodedByteBudget, 1);
    static_cast<void>(runtime.addPointLayer(hierarchy.scene));

    pci::RenderViewportWidget viewport(
        false, gpuByteBudget, gpuTestGraphicsApi(), gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    std::vector<pci::RenderLoadProgress> loadProgress;
    QString failure;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.setLoadProgressCallback(
        [&loadProgress](const pci::RenderLoadProgress &progress) {
            loadProgress.push_back(progress);
        });
    runtime.setDocument(viewport, true);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (latestMetrics &&
                    latestMetrics->submittedPoints ==
                        GpuStressHierarchySource::leafPointCount &&
                    hierarchy.source->metrics().completed == 8 &&
                    std::ranges::any_of(
                        loadProgress,
                        [](const pci::RenderLoadProgress &progress) {
                            return progress.stage ==
                                   pci::RenderLoadStage::DisplayReady;
                        }));
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(latestMetrics);
    CHECK(latestMetrics->requestedPoints ==
          GpuStressHierarchySource::leafPointCount);
    CHECK(latestMetrics->selectedPoints ==
          GpuStressHierarchySource::leafPointCount);
    CHECK(latestMetrics->drawCalls == 8);
    CHECK(hierarchy.source->metrics().completed == 8);
    CHECK(runtime.runtime().decodedResidentBytes() <= decodedByteBudget);
    CHECK(latestMetrics->gpuPointBytes <= gpuByteBudget);
    const auto displayReady = std::ranges::find_if(
        loadProgress, [](const pci::RenderLoadProgress &progress) {
            return progress.stage == pci::RenderLoadStage::DisplayReady;
        });
    REQUIRE(displayReady != loadProgress.end());
    CHECK(displayReady->completed == GpuStressHierarchySource::pointsPerNode);
    CHECK(displayReady->total == GpuStressHierarchySource::leafPointCount);
    REQUIRE(waitForStableFrameCount(viewport));

    const QPoint center(viewport.width() / 2, viewport.height() / 2);
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseMove(&viewport, center + QPoint(12, 0));
    QTest::mouseRelease(
        &viewport, Qt::LeftButton, Qt::NoModifier, center + QPoint(12, 0));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics &&
                   latestMetrics->submittedPoints ==
                       GpuStressHierarchySource::leafPointCount;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    CHECK(latestMetrics->requestedPoints ==
          GpuStressHierarchySource::leafPointCount);
    CHECK(latestMetrics->selectedPoints ==
          GpuStressHierarchySource::leafPointCount);
    CHECK(hierarchy.source->metrics().completed == 8);

    const auto verifyWheelZoom = [&](const int angleDelta) {
        QWheelEvent wheel(QPointF(center),
                          QPointF(viewport.mapToGlobal(center)),
                          {},
                          {0, angleDelta},
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::ScrollUpdate,
                          false);
        QApplication::sendEvent(&viewport, &wheel);
        REQUIRE(QTest::qWaitFor(
            [&] {
                return latestMetrics &&
                       latestMetrics->selectedPoints ==
                           GpuStressHierarchySource::leafPointCount;
            },
            2000));
        REQUIRE(waitForStableFrameCount(viewport));
        CHECK(latestMetrics->requestedPoints ==
              GpuStressHierarchySource::leafPointCount);
        CHECK(latestMetrics->submittedPoints ==
              GpuStressHierarchySource::leafPointCount);
        CHECK(hierarchy.source->metrics().completed == 8);
    };
    verifyWheelZoom(120);
    verifyWheelZoom(-120);

    const std::uint64_t settledFrames =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    viewport.requestRender();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >
                   settledFrames;
        },
        2000));
    REQUIRE(waitForStableFrameCount(viewport));
    REQUIRE(latestMetrics);
    CHECK(latestMetrics->submittedPoints ==
          GpuStressHierarchySource::leafPointCount);
    CHECK(hierarchy.source->metrics().completed == 8);
}

TEST_CASE("GPU asynchronously published hierarchies recover the bootstrap "
          "budget",
          "[gpu][hierarchy][budget][regression]")
{
    constexpr std::uint64_t reportedSourcePoints = 19'000'000;
    GpuStressHierarchy hierarchy =
        makeGpuStressHierarchy(0xffffffffU, reportedSourcePoints);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);

    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    QString failure;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });

    // MainWindow attaches the initially empty document, then publishes the
    // asynchronously loaded scene through updateDocument(). Reproduce that
    // path instead of installing a populated document in one operation.
    runtime.setDocument(viewport, false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   pci::testAccess(viewport).renderedFrameCountForTesting() > 0;
        },
        2000));
    REQUIRE(failure.isEmpty());

    static_cast<void>(runtime.addPointLayer(hierarchy.scene));
    runtime.updateDocument(viewport);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (latestMetrics &&
                    latestMetrics->sourcePoints == reportedSourcePoints &&
                    latestMetrics->requestedPoints == reportedSourcePoints);
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(latestMetrics);
    CHECK(latestMetrics->sourcePoints == reportedSourcePoints);
    CHECK(latestMetrics->requestedPoints == reportedSourcePoints);

    const std::uint64_t settledFrame = latestMetrics->submittedFrameCount;
    pci::testAccess(viewport).orbitCameraForTesting(8.0, 0.0);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() ||
                   (latestMetrics &&
                    latestMetrics->submittedFrameCount > settledFrame);
        },
        2000));
    REQUIRE(failure.isEmpty());
    CHECK(latestMetrics->requestedPoints == reportedSourcePoints);
}

TEST_CASE("GPU hierarchy churn remains bounded and reloads evicted nodes",
          "[gpu][hierarchy][residency][stress]")
{
    GpuStressHierarchy first = makeGpuStressHierarchy(0xffff8080U);
    GpuStressHierarchy second = makeGpuStressHierarchy(0xff80ffffU);
    const std::uint64_t payloadBytes =
        pci::pointCloudNodePayloadBytes(*first.root);
    const std::uint64_t decodedByteBudget = 10 * payloadBytes;
    const std::uint64_t gpuByteBudget =
        9 * GpuStressHierarchySource::pointsPerNode * sizeof(pci::GpuPoint);

    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document, decodedByteBudget, 1);
    const pci::PointCloudLayerId firstLayer =
        runtime.addPointLayer(first.scene);
    const pci::PointCloudLayerId secondLayer =
        runtime.addPointLayer(second.scene, false);

    pci::RenderViewportWidget viewport(
        false, gpuByteBudget, gpuTestGraphicsApi(), gpuTestValidation);
    viewport.resize(320, 240);
    std::optional<pci::RenderMetrics> latestMetrics;
    QString failure;
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    runtime.setDocument(viewport, true);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    const auto waitForRefinement =
        [&](const GpuStressHierarchySource &activeSource,
            const std::uint64_t minimumCompleted) {
            const bool refined = QTest::qWaitFor(
                [&] {
                    return !failure.isEmpty() ||
                           (activeSource.metrics().completed >=
                                minimumCompleted &&
                            latestMetrics &&
                            latestMetrics->submittedPoints ==
                                GpuStressHierarchySource::leafPointCount &&
                            latestMetrics->drawCalls == 8);
                },
                5000);
            INFO("minimum completed: " << minimumCompleted);
            INFO("source completed: " << activeSource.metrics().completed);
            INFO("failure: " << failure.toStdString());
            if (latestMetrics) {
                INFO("submitted points: " << latestMetrics->submittedPoints);
                INFO("draw calls: " << latestMetrics->drawCalls);
                INFO("decoded bytes: " << latestMetrics->decodedPointBytes);
                INFO("GPU bytes: " << latestMetrics->gpuPointBytes);
            }
            REQUIRE(refined);
            REQUIRE(failure.isEmpty());
            REQUIRE(latestMetrics);
            CHECK(latestMetrics->gpuPointBytes <= gpuByteBudget);
            CHECK(latestMetrics->peakGpuPointBytes <= gpuByteBudget);
            CHECK(runtime.runtime().decodedResidentBytes() <=
                  decodedByteBudget);
            CHECK(waitForStableFrameCount(viewport));
        };

    waitForRefinement(*first.source, 8);
    constexpr std::uint64_t churnCycles = 12;
    for (std::uint64_t cycle = 0; cycle < churnCycles; ++cycle) {
        const bool showFirst = cycle % 2 != 0;
        const pci::PointCloudLayerId hide =
            showFirst ? secondLayer : firstLayer;
        const pci::PointCloudLayerId show =
            showFirst ? firstLayer : secondLayer;
        GpuStressHierarchySource &activeSource =
            showFirst ? *first.source : *second.source;
        const std::uint64_t expectedCompleted =
            activeSource.metrics().completed + 8;
        REQUIRE(runtime.setPointLayerVisible(hide, false));
        REQUIRE(runtime.setPointLayerVisible(show, true));
        runtime.updateDocument(viewport);
        waitForRefinement(activeSource, expectedCompleted);
    }

    REQUIRE(latestMetrics);
    CHECK(latestMetrics->gpuPointBytes <= gpuByteBudget);
    CHECK(latestMetrics->peakGpuPointBytes <= gpuByteBudget);
    CHECK(latestMetrics->gpuCacheEvictions >= churnCycles * 8);
    CHECK(latestMetrics->decodedPointBytes <= decodedByteBudget);
    const pci::SceneRuntimeMetrics hierarchy = runtime.runtime().metrics();
    // Cache insertion publishes one complete immutable page before choosing
    // an eviction victim. The global cache may therefore peak by one measured
    // page, but resident steady state remains within the configured budget.
    CHECK(std::max(hierarchy.cache.peakResidentBytes,
                   hierarchy.memoryBudget.peakReservedBytes) <=
          decodedByteBudget + payloadBytes);
    CHECK(hierarchy.cache.evictions >= churnCycles * 8);
    CHECK(hierarchy.source.completed >= 104);
    CHECK(hierarchy.source.failed == 0);
    CHECK(hierarchy.decodeRequestsFailed == 0);
    CHECK(first.source->metrics().completed >= 56);
    CHECK(second.source->metrics().completed >= 48);
}

TEST_CASE("GPU mouse dragging renders continuously and settles on release",
          "[gpu][idle][navigation]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(waitForStableFrameCount(viewport));

    const std::uint64_t settled =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    const QPoint center(viewport.width() / 2, viewport.height() / 2);
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseMove(&viewport, center + QPoint(12, 0));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return pci::testAccess(viewport).renderedFrameCountForTesting() >=
                   settled + 3;
        },
        2000));
    QTest::mouseRelease(
        &viewport, Qt::LeftButton, Qt::NoModifier, center + QPoint(12, 0));
    REQUIRE(waitForStableFrameCount(viewport));
    const std::uint64_t released =
        pci::testAccess(viewport).renderedFrameCountForTesting();
    QTest::qWait(100);
    CHECK(pci::testAccess(viewport).renderedFrameCountForTesting() == released);
}

TEST_CASE("GPU point picking reports hits and misses", "[gpu]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);

    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    bool completed = false;
    std::optional<std::uint32_t> picked;
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            completed = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return completed || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(picked.has_value());
    CHECK(*picked == 0);

    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{-1.0, -1.0, 0.0}})),
        false);
    completed = false;
    picked.reset();
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            completed = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return completed || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    CHECK_FALSE(picked.has_value());
}

TEST_CASE("GPU measurement commits snapped points", "[gpu][measurement]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);

    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.setActiveTool(pci::ViewportTool::Measure);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    const QPoint center(viewport.width() / 2, viewport.height() / 2);

    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, center);
    REQUIRE(waitForStableFrameCount(viewport));
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, center);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !failure.isEmpty() || pci::testAccess(viewport)
                                             .measurementForTesting()
                                             .has_value();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(pci::testAccess(viewport).measurementForTesting());
    CHECK(pci::testAccess(viewport).measurementForTesting()->distance3d ==
          Catch::Approx(0.0));
    CHECK(
        pci::testAccess(viewport).measurementForTesting()->horizontalDistance ==
        Catch::Approx(0.0));
}

TEST_CASE("GPU picking maps ids across multiple blocks", "[gpu]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    // Two single-point blocks: one centred (in front of the default
    // camera), one far off to the side.
    pci::test::setTestDocument(viewport,
                               documentWithScene(sceneWithBlocksAt({
                                   {0.0, 0.0, 0.0},
                                   {0.9, 0.0, 0.9},
                               })),
                               false);

    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    bool completed = false;
    std::optional<std::uint32_t> picked;
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            completed = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return completed || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(picked.has_value());
}

TEST_CASE("GPU picking uses global ids across point-cloud layers", "[gpu]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    // The off-centre point is closer to the camera and receives id 0. The
    // centred point belongs to a second layer and must receive id 1.
    pci::test::setTestDocument(viewport,
                               documentWithScenes({
                                   sceneWithBlocksAt({{0.9, -0.5, 0.9}}),
                                   sceneWithBlocksAt({{0.0, 0.0, 0.0}}),
                               }),
                               false);

    QString failure;
    std::optional<pci::RenderMetrics> latestMetrics;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.setMetricsCallback(
        [&latestMetrics](const pci::RenderMetrics &metrics) {
            latestMetrics = metrics;
        });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    bool completed = false;
    std::optional<std::uint32_t> picked;
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            completed = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] {
            return completed || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(picked.has_value());
    CHECK(*picked == 1);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics && latestMetrics->pickInputBlocks == 2 &&
                   latestMetrics->pickCandidateBlocks == 1;
        },
        2000));
    CHECK(latestMetrics->pickInputPoints == 2);
    CHECK(latestMetrics->pickCandidatePoints == 1);
}

TEST_CASE("GPU stale pick readback cannot target a replacement document",
          "[gpu][pick]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);

    QString failure;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(waitForStableFrameCount(viewport));

    bool replacementPickCompleted = false;
    std::optional<std::uint32_t> replacementPick;
    QMetaObject::Connection replacementConnection;
    replacementConnection = QObject::connect(
        &viewport,
        &QRhiWidget::frameSubmitted,
        &viewport,
        [&] {
            QObject::disconnect(replacementConnection);
            pci::test::setTestDocument(
                viewport,
                documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
                false);
            pci::testAccess(viewport).requestRawPickForTesting(
                QPoint(viewport.width() / 2, viewport.height() / 2),
                [&](const std::optional<std::uint32_t> id) {
                    replacementPickCompleted = true;
                    replacementPick = id;
                });
        },
        Qt::DirectConnection);
    pci::testAccess(viewport).requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [](const std::optional<std::uint32_t>) {});

    REQUIRE(QTest::qWaitFor(
        [&] {
            return replacementPickCompleted || !failure.isEmpty();
        },
        5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(replacementPick);
    CHECK(*replacementPick == 0);
}

TEST_CASE("GPU picking survives renderer uniform capacity growth", "[gpu]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithRepeatedBlocksAt({0.0, 0.0, 0.0}, 257)),
        false);

    QString failure;
    std::optional<pci::RenderMetrics> latestMetrics;
    std::uint64_t maximumUploadOperations = 0;
    std::uint64_t maximumUploadBatches = 0;
    std::uint64_t maximumUniformUpdates = 0;
    viewport.setFailureCallback([&failure](const QString &message) {
        failure = message;
    });
    viewport.setMetricsCallback([&latestMetrics,
                                 &maximumUploadOperations,
                                 &maximumUploadBatches,
                                 &maximumUniformUpdates](
                                    const pci::RenderMetrics &metrics) {
        latestMetrics = metrics;
        maximumUploadOperations =
            std::max(maximumUploadOperations, metrics.uploadOperations);
        maximumUploadBatches =
            std::max(maximumUploadBatches, metrics.uploadResourceUpdateBatches);
        maximumUniformUpdates =
            std::max(maximumUniformUpdates, metrics.uniformUpdateOperations);
    });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    const auto verifyCenterPick = [&] {
        bool completed = false;
        std::optional<std::uint32_t> picked;
        pci::testAccess(viewport).requestRawPickForTesting(
            QPoint(viewport.width() / 2, viewport.height() / 2),
            [&](const std::optional<std::uint32_t> id) {
                completed = true;
                picked = id;
            });
        REQUIRE(QTest::qWaitFor(
            [&] {
                return completed || !failure.isEmpty();
            },
            5000));
        REQUIRE(failure.isEmpty());
        REQUIRE(picked.has_value());
        CHECK(*picked == 0);
    };

    // The initial renderer allocation holds 256 draw uniforms. This scene
    // forces the first replacement after the pick pipeline has been created.
    verifyCenterPick();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics && latestMetrics->uniformDrawCapacity == 512 &&
                   latestMetrics->submittedPoints == 257;
        },
        2000));
    CHECK(latestMetrics->selectedPoints == 257);
    CHECK(latestMetrics->drawCalls == 257);
    CHECK(latestMetrics->uniformCapacityGrowthCount == 1);
    CHECK(maximumUniformUpdates >= 1);
    CHECK(maximumUploadOperations == 257);
    CHECK(maximumUploadBatches >= 1);
    CHECK(maximumUploadBatches < maximumUploadOperations);

    maximumUploadOperations = 0;
    maximumUploadBatches = 0;
    maximumUniformUpdates = 0;
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithRepeatedBlocksAt({0.0, 0.0, 0.0}, 513)),
        false);

    // The first replacement grows to 512 entries; verify a second replacement
    // also supplies the picker with the renderer's current binding set.
    verifyCenterPick();
    REQUIRE(QTest::qWaitFor(
        [&] {
            return latestMetrics &&
                   latestMetrics->uniformDrawCapacity == 1024 &&
                   latestMetrics->submittedPoints == 513;
        },
        2000));
    CHECK(latestMetrics->selectedPoints == 513);
    CHECK(latestMetrics->drawCalls == 513);
    CHECK(latestMetrics->uniformCapacityGrowthCount == 2);
    CHECK(maximumUniformUpdates >= 1);
    CHECK(maximumUploadOperations == 513);
    CHECK(maximumUploadBatches >= 1);
    CHECK(maximumUploadBatches < maximumUploadOperations);
}

} // namespace

TEST_CASE("GPU pending pick storage survives release and recreation",
          "[gpu][pick][lifetime]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(waitForStableFrameCount(viewport));
    auto access = pci::testAccess(viewport);
    auto *rhi = access.rhiForTesting();
    const QSize pixelSize = viewport.size() * viewport.devicePixelRatioF();
    pci::PointPicker picker;
    bool oldDelivered = false;
    QRhiCommandBuffer *commands = nullptr;
    REQUIRE(rhi->beginOffscreenFrame(&commands) == QRhi::FrameOpSuccess);
    access.recordSinglePointPickForTesting(
        commands, picker, pixelSize, [&](auto) {
            oldDelivered = true;
        });
    REQUIRE(picker.inFlight());
    picker.releaseResources();
    REQUIRE(rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
    CHECK_FALSE(oldDelivered);
    CHECK_FALSE(picker.inFlight());

    bool newDelivered = false;
    REQUIRE(rhi->beginOffscreenFrame(&commands) == QRhi::FrameOpSuccess);
    access.recordSinglePointPickForTesting(
        commands, picker, pixelSize, [&](auto) {
            newDelivered = true;
        });
    REQUIRE(picker.inFlight());
    REQUIRE(rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
    CHECK(newDelivered);
}

TEST_CASE("GPU pending pick storage survives picker destruction",
          "[gpu][pick][lifetime]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        gpuTestGraphicsApi(),
        gpuTestValidation);
    viewport.resize(320, 240);
    pci::test::setTestDocument(
        viewport,
        documentWithScene(sceneWithBlocksAt({{0.0, 0.0, 0.0}})),
        false);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));
    REQUIRE(waitForStableFrameCount(viewport));
    auto access = pci::testAccess(viewport);
    auto *rhi = access.rhiForTesting();
    auto picker = std::make_unique<pci::PointPicker>();
    bool delivered = false;
    QRhiCommandBuffer *commands = nullptr;
    REQUIRE(rhi->beginOffscreenFrame(&commands) == QRhi::FrameOpSuccess);
    access.recordSinglePointPickForTesting(commands,
                                           *picker,
                                           viewport.size() *
                                               viewport.devicePixelRatioF(),
                                           [&](auto) {
                                               delivered = true;
                                           });
    REQUIRE(picker->inFlight());
    picker.reset();
    REQUIRE(rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
    CHECK_FALSE(delivered);
}
