#include "renderer/planning/RasterLodPlanner.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

class PlannerRasterSource final : public pci::RasterTileSource {
public:
    explicit PlannerRasterSource(pci::RasterLayerMetadata metadata)
        : metadata_(std::move(metadata))
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
        throw pci::RasterReadError("the planner fixture holds no pixels");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

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

// Deliberately non-power-of-two reductions, so nothing downstream can assume a
// 2^L pyramid.
[[nodiscard]] pci::RasterLayer plannerLayer(const double pixelSize = 1.0)
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 2048;
    metadata.height = 2048;
    metadata.geoTransform = {0.0, pixelSize, 0.0, 0.0, 0.0, -pixelSize};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.levels = {
        makeLevel(2048, 2048, 2048, 2048),
        makeLevel(683, 683, 2048, 2048),
        makeLevel(228, 228, 2048, 2048),
    };

    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{1};
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<PlannerRasterSource>(std::move(metadata)),
    });
    return layer;
}

// Looks straight down at the raster plane from the given height, centred on
// the raster. With a 60 degree vertical field of view over 1000 pixels, one
// world unit at height h spans 1 / (0.001155 * h) screen pixels, which is what
// puts the level transitions at the heights the sweep below relies on.
[[nodiscard]] pci::FrameCamera overheadCamera(const double height,
                                              const double centerX = 1024.0,
                                              const double centerY = -1024.0)
{
    pci::FrameCamera camera;
    camera.eye = {centerX, centerY, height};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.right = {1.0, 0.0, 0.0};
    camera.outputWidth = 1000;
    camera.outputHeight = 1000;
    camera.nearPlane = 1.0;
    camera.farPlane = 100000.0;
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

[[nodiscard]] pci::RasterLodPlanInput planInput(const pci::RasterLayer &layer,
                                                const pci::FrameCamera &camera)
{
    pci::RasterLodPlanInput input;
    input.layer = layer;
    input.camera = camera;
    input.cpuResident = [](pci::RasterTileKey) {
        return false;
    };
    input.gpuResident = [](pci::RasterTileKey) {
        return false;
    };
    return input;
}

TEST_CASE("raster planner selects nothing when the layer is off screen",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::FrameCamera camera = overheadCamera(2000.0);
    // Point the camera away from the raster entirely.
    camera.eye = {1024.0, -1024.0, -2000.0};
    camera.culler = pci::FrustumCuller::fromCamera(camera.eye,
                                                   camera.forward,
                                                   camera.up,
                                                   camera.right,
                                                   camera.verticalFovDegrees,
                                                   1.0,
                                                   camera.nearPlane,
                                                   camera.farPlane);

    const pci::RasterLodPlan plan =
        pci::planRasterTiles(planInput(layer, camera));
    CHECK(plan.selected.empty());
    CHECK(plan.requests.empty());
    CHECK(plan.draw.empty());
}

TEST_CASE("raster planner refines as the camera approaches",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();

    const pci::RasterLodPlan far =
        pci::planRasterTiles(planInput(layer, overheadCamera(4000.0)));
    const pci::RasterLodPlan near =
        pci::planRasterTiles(planInput(layer, overheadCamera(200.0)));

    REQUIRE_FALSE(far.selected.empty());
    REQUIRE_FALSE(near.selected.empty());

    const auto finestLevel = [](const pci::RasterLodPlan &plan) {
        std::uint32_t finest = std::numeric_limits<std::uint32_t>::max();
        for (const pci::RasterTileKey key : plan.selected) {
            finest = std::min(finest, key.levelIndex);
        }
        return finest;
    };
    // Closer means finer, and a close view clamps to level 0 so one texel is
    // one native source pixel.
    CHECK(finestLevel(near) < finestLevel(far));
    CHECK(finestLevel(near) == 0);
}

TEST_CASE("raster planner honours the smaller of the two caps",
          "[renderer][raster][lod]")
{
    // Five-centimetre pixels, so the whole raster resolves to level 0 from a
    // height that still sees all of it: many tiles selected at once.
    const pci::RasterLayer layer = plannerLayer(0.05);
    const pci::FrameCamera camera = overheadCamera(150.0, 51.2, -51.2);

    pci::RasterLodPlanInput input = planInput(layer, camera);
    input.maximumSelectedTiles = 4;
    input.gpuCapacityTiles = 512;
    const pci::RasterLodPlan plannerLimited = pci::planRasterTiles(input);
    CHECK(plannerLimited.selected.size() <= 4);
    CHECK(plannerLimited.capacityLimited);
    CHECK_FALSE(plannerLimited.coverageIncomplete);

    // Driving the other limit below the first must bind just as tightly: the
    // two are independent ceilings and the planner always takes the smaller.
    input.maximumSelectedTiles = 512;
    input.gpuCapacityTiles = 4;
    const pci::RasterLodPlan budgetLimited = pci::planRasterTiles(input);
    CHECK(budgetLimited.selected.size() <= 4);
    CHECK(budgetLimited.capacityLimited);
    CHECK_FALSE(budgetLimited.coverageIncomplete);

    input.maximumSelectedTiles = 512;
    input.gpuCapacityTiles = 512;
    CHECK(pci::planRasterTiles(input).selected.size() > 4);
}

TEST_CASE("raster planner is stable across identical frames",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();
    const pci::FrameCamera camera = overheadCamera(600.0);

    const pci::RasterLodPlan first =
        pci::planRasterTiles(planInput(layer, camera));
    REQUIRE_FALSE(first.selected.empty());

    pci::RasterLodPlanInput second = planInput(layer, camera);
    second.previousSelection = first.selected;
    const pci::RasterLodPlan repeated = pci::planRasterTiles(second);

    // A still camera must reproduce its selection exactly. A key-equality
    // prior-level lookup passes this case and fails the sweep below.
    CHECK(repeated.selected == first.selected);
}

TEST_CASE("raster planner does not oscillate across a level boundary",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();

    // Sweep across both level transitions, feeding each frame's selection into
    // the next. Level 1 becomes adequate above about 3200 and level 0 becomes
    // necessary below about 690, so this range crosses both.
    std::vector<pci::RasterTileKey> previous;
    std::vector<std::uint32_t> observed;
    for (int step = 0; step <= 40; ++step) {
        const double height = 8000.0 - static_cast<double>(step) * 195.0;
        pci::RasterLodPlanInput input =
            planInput(layer, overheadCamera(height));
        input.previousSelection = previous;
        const pci::RasterLodPlan plan = pci::planRasterTiles(input);
        REQUIRE_FALSE(plan.selected.empty());

        std::uint32_t finest = std::numeric_limits<std::uint32_t>::max();
        for (const pci::RasterTileKey key : plan.selected) {
            finest = std::min(finest, key.levelIndex);
        }
        observed.push_back(finest);
        previous = plan.selected;
    }

    // Approaching monotonically must never send a region back to a coarser
    // level: that reversal is the flapping the candidate-level rule exists to
    // prevent.
    for (std::size_t index = 1; index < observed.size(); ++index) {
        CHECK(observed[index] <= observed[index - 1]);
    }
    CHECK(observed.front() > observed.back());
}

TEST_CASE("raster planner falls back to a resident coarse ancestor",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();
    const pci::FrameCamera camera = overheadCamera(300.0);

    // Only the coarsest level is resident, which is the state right after a
    // zoom: the target detail is still being read.
    pci::RasterLodPlanInput input = planInput(layer, camera);
    input.gpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 2;
    };

    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE_FALSE(plan.selected.empty());
    REQUIRE_FALSE(plan.draw.empty());

    // Every drawn tile is resident, and the coarse ancestor keeps the region
    // covered rather than leaving a hole while children load.
    for (const pci::RasterTileKey key : plan.draw) {
        CHECK(key.levelIndex == 2);
    }
    // Fallback ancestors are protected so admission cannot evict what is on
    // screen this frame.
    for (const pci::RasterTileKey key : plan.draw) {
        CHECK(std::ranges::find(plan.protectedTiles, key) !=
              plan.protectedTiles.end());
    }
    CHECK_FALSE(plan.requests.empty());
}

TEST_CASE("raster planner reuploads decoded target tiles without rereading",
          "[renderer][raster][lod][residency]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(300.0));
    input.cpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 0;
    };

    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE_FALSE(plan.selected.empty());
    CHECK(plan.requests.empty());
    CHECK(plan.decodedUploads == plan.selected);
    for (const pci::RasterTileKey key : plan.decodedUploads) {
        CHECK(std::ranges::find(plan.protectedTiles, key) !=
              plan.protectedTiles.end());
    }
}

TEST_CASE("raster planner keeps a resident fallback while reuploading detail",
          "[renderer][raster][lod][residency]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(300.0));
    input.cpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 0;
    };
    input.gpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 1;
    };

    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE_FALSE(plan.decodedUploads.empty());
    REQUIRE_FALSE(plan.draw.empty());
    CHECK(plan.requests.empty());
    CHECK(std::ranges::all_of(plan.decodedUploads,
                              [](const pci::RasterTileKey key) {
                                  return key.levelIndex == 0;
                              }));
    CHECK(std::ranges::all_of(plan.draw, [](const pci::RasterTileKey key) {
        return key.levelIndex == 1;
    }));
}

TEST_CASE(
    "raster planner reuploads the nearest decoded arbitrary-ratio ancestor",
    "[renderer][raster][lod][residency]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(300.0));
    input.cpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 1;
    };

    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE_FALSE(plan.decodedUploads.empty());
    CHECK(std::ranges::all_of(plan.decodedUploads,
                              [](const pci::RasterTileKey key) {
                                  return key.levelIndex == 1;
                              }));
    CHECK(std::ranges::none_of(plan.requests, [](const pci::RasterTileKey key) {
        return key.levelIndex > 1;
    }));
}

TEST_CASE("raster planner draws a resident ancestor beyond a decoded fallback",
          "[renderer][raster][lod][residency]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(300.0));
    input.cpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 1;
    };
    input.gpuResident = [](const pci::RasterTileKey key) {
        return key.levelIndex == 2;
    };

    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE_FALSE(plan.decodedUploads.empty());
    REQUIRE_FALSE(plan.draw.empty());
    CHECK(std::ranges::all_of(plan.decodedUploads,
                              [](const pci::RasterTileKey key) {
                                  return key.levelIndex == 1;
                              }));
    CHECK(std::ranges::all_of(plan.draw, [](const pci::RasterTileKey key) {
        return key.levelIndex == 2;
    }));
}

TEST_CASE("raster planner requests coarse coverage before detail",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(700.0));
    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    REQUIRE(plan.requests.size() > 1);
    CHECK(plan.requests.front().levelIndex ==
          layer.data->metadata().levels.size() - 1);

    // Coarse levels come first so the view fills in before it sharpens.
    for (std::size_t index = 1; index < plan.requests.size(); ++index) {
        CHECK(plan.requests[index - 1].levelIndex >=
              plan.requests[index].levelIndex);
    }
}

TEST_CASE("raster planner bounds enumeration across an enormous overview gap",
          "[renderer][raster][lod][stress]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 1'000'000'000;
    metadata.height = 1'000'000'000;
    // The logical source is enormous while its world footprint is ordinary.
    // A one-texel overview is too coarse for the screen and its next finer
    // level contains trillions of tiles. The planner must stop enumeration at
    // the effective cap rather than materialize that candidate set first.
    metadata.geoTransform = {0.0, 1.0e-6, 0.0, 0.0, 0.0, -1.0e-6};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.levels = {
        makeLevel(
            metadata.width, metadata.height, metadata.width, metadata.height),
        makeLevel(1, 1, metadata.width, metadata.height),
    };

    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{7};
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<PlannerRasterSource>(std::move(metadata)),
    });

    pci::RasterLodPlanInput input =
        planInput(layer, overheadCamera(1000.0, 500.0, -500.0));
    input.maximumSelectedTiles = 4;
    input.gpuCapacityTiles = 4;
    const pci::RasterLodPlan plan = pci::planRasterTiles(input);
    CHECK(plan.selected.size() <= 4);
    CHECK(plan.capacityLimited);
    // The cap prevents refinement, but retaining the one-cell root still
    // covers the complete raster footprint.
    CHECK_FALSE(plan.coverageIncomplete);
}

TEST_CASE("raster generated root keeps a 10k raster fully covered",
          "[renderer][raster][lod][coverage]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 10000;
    metadata.height = 10000;
    metadata.geoTransform = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.levels = {makeLevel(10000, 10000, 10000, 10000)};
    pci::appendGeneratedRasterCoverageLevels(
        metadata.levels, metadata.width, metadata.height);

    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{8};
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<PlannerRasterSource>(std::move(metadata)),
    });

    pci::RasterLodPlanInput input =
        planInput(layer, overheadCamera(10000.0, 5000.0, -5000.0));
    input.maximumSelectedTiles = 188;
    input.gpuCapacityTiles = 188;
    const pci::RasterLodPlan plan = pci::planRasterTiles(input);

    REQUIRE_FALSE(plan.selected.empty());
    CHECK(plan.selected.size() <= 188);
    CHECK_FALSE(plan.coverageIncomplete);
    CHECK(plan.requests.front().levelIndex ==
          layer.data->metadata().levels.size() - 1);

    double coveredArea = 0.0;
    for (const pci::RasterTileKey key : plan.selected) {
        const pci::RasterBasePixelRect rect = pci::rasterTileBasePixelRect(
            layer.data->metadata().levels[key.levelIndex], key, 10000, 10000);
        coveredArea += (rect.maximumPixel - rect.minimumPixel) *
                       (rect.maximumLine - rect.minimumLine);
    }
    CHECK(coveredArea == Catch::Approx(10000.0 * 10000.0));
}

TEST_CASE("raster planner bypasses a failed generated coverage tile",
          "[renderer][raster][lod][failure]")
{
    pci::RasterLayer layer = plannerLayer();
    pci::RasterLayerMetadata metadata = layer.data->metadata();
    metadata.levels.resize(1);
    pci::appendGeneratedRasterCoverageLevels(
        metadata.levels, metadata.width, metadata.height);
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<PlannerRasterSource>(std::move(metadata)),
    });

    const std::uint32_t root =
        static_cast<std::uint32_t>(layer.data->metadata().levels.size() - 1);
    pci::RasterLodPlanInput input = planInput(layer, overheadCamera(4000.0));
    input.unavailable = [root](const pci::RasterTileKey key) {
        return key.levelIndex == root;
    };
    const pci::RasterLodPlan plan = pci::planRasterTiles(input);

    REQUIRE_FALSE(plan.selected.empty());
    CHECK(std::ranges::none_of(plan.selected, [root](const auto key) {
        return key.levelIndex == root;
    }));
    CHECK_FALSE(plan.coverageIncomplete);
}

TEST_CASE("raster planner clips a rotated footprint to its visible region",
          "[renderer][raster][lod]")
{
    pci::RasterLayerMetadata metadata;
    metadata.width = 2048;
    metadata.height = 2048;
    // A 30 degree rotation, so the axis-aligned bounds are far larger than the
    // footprint itself.
    metadata.geoTransform = {0.0, 0.866, -0.5, 0.0, 0.5, -0.866};
    metadata.bounds = *pci::rasterPixelEdgeBounds(
        metadata.geoTransform, metadata.width, metadata.height);
    metadata.levels = {makeLevel(2048, 2048, 2048, 2048),
                       makeLevel(683, 683, 2048, 2048)};

    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{2};
    layer.data = std::make_shared<pci::RasterLayerData>(pci::RasterLayerData{
        .sourceId = pci::nextRasterSourceId(),
        .source = std::make_shared<PlannerRasterSource>(std::move(metadata)),
    });

    const std::vector<std::array<double, 2>> polygon =
        pci::rasterVisiblePixelPolygon(layer, overheadCamera(3000.0));
    REQUIRE(polygon.size() >= 3);
    // The mapped polygon stays inside the raster's own pixel domain; a
    // negative or out-of-range coordinate would become a huge unsigned tile
    // index.
    for (const std::array<double, 2> &vertex : polygon) {
        CHECK(vertex[0] >= 0.0);
        CHECK(vertex[1] >= 0.0);
        CHECK(vertex[0] <= 2048.0);
        CHECK(vertex[1] <= 2048.0);
    }
}

TEST_CASE("raster projected texel size scales with distance",
          "[renderer][raster][lod]")
{
    const pci::RasterLayer layer = plannerLayer();
    const pci::RasterLayerMetadata &metadata = layer.data->metadata();

    const double near = pci::rasterProjectedTexelPixels(metadata,
                                                        metadata.levels[0],
                                                        {1024.0, -1024.0, 0.0},
                                                        overheadCamera(100.0));
    const double far = pci::rasterProjectedTexelPixels(metadata,
                                                       metadata.levels[0],
                                                       {1024.0, -1024.0, 0.0},
                                                       overheadCamera(1000.0));
    CHECK(near > far);

    // A coarser level's texel covers more ground and therefore more screen.
    const double coarse =
        pci::rasterProjectedTexelPixels(metadata,
                                        metadata.levels[2],
                                        {1024.0, -1024.0, 0.0},
                                        overheadCamera(100.0));
    CHECK(coarse > near);
}

} // namespace
