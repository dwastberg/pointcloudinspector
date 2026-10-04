#include <pci/rendering/planning/RasterFrameCoordinator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace {

class InertRasterSource final : public pci::RasterTileSource {
public:
    InertRasterSource(pci::RasterLayerMetadata metadata,
                      std::shared_ptr<std::atomic_uint64_t> reads)
        : metadata_(std::move(metadata))
        , reads_(std::move(reads))
    {
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        ++*reads_;
        throw pci::RasterReadError("frame planning must not read tiles");
    }

private:
    pci::RasterLayerMetadata metadata_;
    std::shared_ptr<std::atomic_uint64_t> reads_;
};

struct TestLayer {
    pci::SceneLayerId layerId;
    pci::RasterSourceId sourceId;
    pci::BindingGeneration bindingGeneration;
    std::uint64_t renderGeneration = 1;
    bool visible = true;
    pci::RasterLayerMetadata metadata;
    pci::RasterLayerStyle style;
    pci::RasterElevationStatus elevationStatus =
        pci::RasterElevationStatus::NotApplicable;
    std::optional<pci::RasterElevationRange> exactElevationRange;
    pci::RasterTileSourcePtr source;
};

[[nodiscard]] pci::RasterLevel level(const std::uint32_t size,
                                     const std::uint32_t baseSize)
{
    pci::RasterLevel result;
    result.width = size;
    result.height = size;
    result.basePixelsPerTexelX =
        static_cast<double>(baseSize) / static_cast<double>(size);
    result.basePixelsPerTexelY = result.basePixelsPerTexelX;
    result.channelCount = 3;
    return result;
}

[[nodiscard]] TestLayer
makeLayer(const std::uint64_t layerValue,
          const std::shared_ptr<std::atomic_uint64_t> &reads)
{
    TestLayer layer;
    layer.layerId = pci::SceneLayerId{layerValue};
    layer.sourceId = pci::nextRasterSourceId();
    layer.bindingGeneration = pci::BindingGeneration{layerValue};
    layer.renderGeneration = layerValue + 10;
    layer.metadata.width = 1024;
    layer.metadata.height = 1024;
    layer.metadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    layer.metadata.bounds =
        *pci::rasterPixelEdgeBounds(layer.metadata.geoTransform,
                                    layer.metadata.width,
                                    layer.metadata.height);
    layer.metadata.levels = {
        level(1024, 1024), level(256, 1024), level(64, 1024)};
    layer.source = std::make_shared<InertRasterSource>(layer.metadata, reads);
    return layer;
}

[[nodiscard]] pci::FrameCamera overheadCamera()
{
    pci::FrameCamera camera;
    camera.eye = {512.0, -512.0, 900.0};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.right = {1.0, 0.0, 0.0};
    camera.outputWidth = 1000;
    camera.outputHeight = 1000;
    camera.nearPlane = 1.0;
    camera.farPlane = 10000.0;
    camera.verticalFovDegrees = 60.0;
    camera.culler = pci::FrustumCuller::fromCamera(camera.eye,
                                                   camera.forward,
                                                   camera.up,
                                                   camera.right,
                                                   camera.verticalFovDegrees,
                                                   1.0,
                                                   camera.nearPlane,
                                                   camera.farPlane);
    return camera;
}

[[nodiscard]] pci::RasterFrameLayerInput layerInput(const TestLayer &layer)
{
    return {
        .layerId = layer.layerId,
        .sourceId = layer.sourceId,
        .bindingGeneration = layer.bindingGeneration,
        .renderGeneration = layer.renderGeneration,
        .visible = layer.visible,
        .layer = pci::RasterLodLayerView{layer.metadata,
                                         layer.style,
                                         layer.elevationStatus,
                                         layer.exactElevationRange},
        .sourceAvailable = bool(layer.source),
        .decode = {},
    };
}

[[nodiscard]] pci::RasterFrameInput
frameInput(const std::span<const pci::RasterFrameLayerInput> layers,
           const pci::RasterSourceId decodedSource = {})
{
    return {
        .layers = layers,
        .camera = overheadCamera(),
        .colorTileCapacity = 8,
        .elevationTileCapacity = 4,
        .surfaceSupported = false,
        .cpuResident =
            [decodedSource](const pci::RasterCacheKey &key) {
                return key.sourceId == decodedSource;
            },
        .gpuResident =
            [](const pci::RasterCacheKey &) {
                return false;
            },
        .unavailable =
            [](const pci::RasterCacheKey &) {
                return false;
            },
    };
}

TEST_CASE("raster frame coordination is effect free and frame global",
          "[renderer][raster][frame][architecture]")
{
    const auto reads = std::make_shared<std::atomic_uint64_t>(0);
    const TestLayer first = makeLayer(1, reads);
    const TestLayer second = makeLayer(2, reads);
    const std::vector<pci::RasterFrameLayerInput> inputs{layerInput(first),
                                                         layerInput(second)};

    pci::RasterFrameCoordinator coordinator;
    const pci::RasterFramePlan plan =
        coordinator.buildPlan(frameInput(inputs, second.sourceId));

    CHECK(reads->load() == 0);
    REQUIRE(plan.layers.size() == 2);
    REQUIRE(plan.requests.size() == 2);
    CHECK(plan.retainedLayers.size() == 2);
    CHECK(plan.liveSources.size() == 2);
    CHECK(plan.uploadTargets.size() == 2);
    CHECK(plan.statistics.visibleLayers == 2);
    CHECK(plan.statistics.selectedTiles > 0);
    CHECK(plan.layers[0].selected.size() <= 4);
    CHECK(plan.layers[1].selected.size() <= 4);
    CHECK_FALSE(plan.requests[0].orderedRequests.empty());
    CHECK(plan.requests[1].orderedRequests.empty());
    CHECK_FALSE(plan.protectedTiles.empty());
    for (const pci::RasterCacheKey &key : plan.decodedUploads) {
        CHECK(std::ranges::find(plan.protectedTiles, key) !=
              plan.protectedTiles.end());
    }
    CHECK(plan.requests[0].renderGeneration == first.renderGeneration);
    CHECK(plan.requests[0].bindingGeneration == first.bindingGeneration);
}

TEST_CASE("raster frame coordination publishes empty desires for hidden layers",
          "[renderer][raster][frame]")
{
    const auto reads = std::make_shared<std::atomic_uint64_t>(0);
    TestLayer first = makeLayer(1, reads);
    TestLayer second = makeLayer(2, reads);
    const std::vector<pci::RasterFrameLayerInput> visible{layerInput(first),
                                                          layerInput(second)};

    pci::RasterFrameCoordinator coordinator;
    static_cast<void>(coordinator.buildPlan(frameInput(visible)));

    first.visible = false;
    const std::vector<pci::RasterFrameLayerInput> next{layerInput(first),
                                                       layerInput(second)};
    const pci::RasterFramePlan hidden = coordinator.buildPlan(frameInput(next));

    REQUIRE(hidden.layers.size() == 1);
    REQUIRE(hidden.requests.size() == 2);
    CHECK(hidden.requests[0].sourceId == first.sourceId);
    CHECK(hidden.requests[0].orderedRequests.empty());
    CHECK_FALSE(hidden.requests[1].orderedRequests.empty());
    CHECK(hidden.statistics.visibleLayers == 1);
    CHECK(reads->load() == 0);
}

} // namespace
