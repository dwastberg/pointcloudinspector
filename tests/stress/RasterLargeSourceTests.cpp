#include "fixtures/GdalRasterFixtureFactory.h"
#include "import/gdal/GdalRasterLoader.h"
#include "import/gdal/GdalRasterSource.h"
#include "import/gdal/GdalRuntime.h"
#include "renderer/planning/RasterLodPlanner.h"
#include "renderer/rhi/RasterTileStreamer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace {

// The corpus is fixed in size rather than scaled to available memory. A
// memory-relative fixture behaves differently on CI than on a workstation and
// turns a pass/fail bound into a coin flip.
const pci::test::GdalRasterFixturePaths &fixtures()
{
    static const pci::test::GdalRasterFixturePaths paths =
        pci::test::writeGdalRasterFixtures(
            std::filesystem::temp_directory_path() / "pci-raster-stress");
    return paths;
}

[[nodiscard]] pci::RasterLayerDataPtr load(const std::filesystem::path &path,
                                           const pci::GdalRasterLoader &loader)
{
    pci::RasterImportRequest request;
    request.sourcePath = path;
    return loader.inspect(request).data;
}

[[nodiscard]] pci::RasterLayer layerFor(pci::RasterLayerDataPtr data)
{
    pci::RasterLayer layer;
    layer.id = pci::SceneLayerId{1};
    layer.renderGeneration = 1;
    layer.data = std::move(data);
    layer.style = pci::defaultRasterLayerStyle(layer.data->metadata());
    return layer;
}

// Looks straight down at a world position from the given height.
[[nodiscard]] pci::FrameCamera overheadCamera(const pci::Vec3d target,
                                              const double height)
{
    pci::FrameCamera camera;
    camera.eye = {target.x, target.y, height};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.right = {1.0, 0.0, 0.0};
    camera.outputWidth = 1000;
    camera.outputHeight = 1000;
    camera.nearPlane = 1.0;
    camera.farPlane = 1.0e9;
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

TEST_CASE("a huge catalog plans and reads a bounded amount of work",
          "[component][raster][stress]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayer layer = layerFor(load(fixtures().catalog, loader));
    const pci::RasterLayerMetadata &metadata = layer.data->metadata();
    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(layer.data->source.get());
    REQUIRE(source != nullptr);

    // About 4e10 logical pixels. Every bound below is an absolute number, so
    // any behaviour proportional to that figure fails rather than merely
    // running slowly.
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(metadata.width) * metadata.height;
    REQUIRE(pixels > 40'000'000'000ULL);

    // Inspection is bounded: opening the catalog must not scan its members.
    CHECK(loader.sampleReadCount() <= 64);

    const pci::Vec3d center{
        (metadata.bounds.minimum[0] + metadata.bounds.maximum[0]) * 0.5,
        (metadata.bounds.minimum[1] + metadata.bounds.maximum[1]) * 0.5,
        0.0};

    pci::RasterTileStreamer streamer(64ULL * 1024 * 1024, 2);
    const std::uint64_t readsBeforePlanning = source->readCount();

    // Whole-catalog view. The planner's own ceiling has to hold here: the
    // coarsest level still needs hundreds of thousands of tiles to cover.
    const pci::RasterLodPlan wide =
        planRasterTiles(planInput(layer, overheadCamera(center, 900000.0)));
    // The cap binds exactly rather than merely not being exceeded: a plan
    // that quietly selected nothing would also satisfy an upper bound.
    CHECK(wide.selected.size() == 512);
    CHECK(wide.capacityLimited);
    CHECK(wide.insufficientOverviews);
    // Planning reads nothing; it is arithmetic over the level table.
    CHECK(source->readCount() == readsBeforePlanning);

    streamer.reconcile(wide, layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const pci::RasterStreamerMetrics metrics = streamer.metrics();
    CHECK(metrics.cpuBytes <= 64ULL * 1024 * 1024);
    // The queue ceiling binds too: 512 wanted tiles become 256 accepted.
    CHECK(metrics.requested == pci::rasterMaximumPendingRequests);
    CHECK(metrics.completed > 0);
    // One window per band per requested tile, and nothing else. This is the
    // assertion that fails if anything starts scanning the catalog: it is an
    // absolute count, unrelated to the source's 4e10 pixels.
    CHECK(source->readCount() - readsBeforePlanning ==
          metrics.requested * metadata.bands.size());

    // GDAL's own cache stays inside the limit the application set, which is
    // the third allocator the accounting would otherwise miss.
    pci::setGdalBlockCacheBytes(32ULL * 1024 * 1024);
    CHECK(pci::gdalBlockCacheUsedBytes() <= 32ULL * 1024 * 1024);
}

TEST_CASE("zooming into a catalog member reaches its native pixels",
          "[component][raster][stress]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayer layer = layerFor(load(fixtures().catalog, loader));
    const pci::RasterLayerMetadata &metadata = layer.data->metadata();

    // The first member sits at the catalog's north-west corner. Its pixels are
    // 2 m, so a camera low enough for one texel to exceed a screen pixel must
    // select the native level.
    const pci::Vec3d member{metadata.bounds.minimum[0] + 500.0,
                            metadata.bounds.maximum[1] - 500.0,
                            0.0};
    const pci::RasterLodPlan close =
        planRasterTiles(planInput(layer, overheadCamera(member, 800.0)));

    REQUIRE_FALSE(close.selected.empty());
    // A 1 km view over 2 m pixels is four native tiles, not a fraction of the
    // catalog's 600000-pixel span.
    CHECK(close.selected.size() == 4);
    // Level 0 is mandatory: a source without overviews has nowhere coarser to
    // stop, and clamping short of it would cap achievable detail forever.
    CHECK(std::ranges::all_of(close.selected, [](const pci::RasterTileKey key) {
        return key.levelIndex == 0;
    }));
    CHECK(close.selected.size() <= 512);

    // The tiles named are the ones covering the member, not the catalog.
    pci::RasterTileStreamer streamer(64ULL * 1024 * 1024, 2);
    streamer.reconcile(close, layer);
    streamer.waitForIdle();
    const std::vector<pci::RasterCacheKey> admitted =
        streamer.drainCompletions({});
    REQUIRE_FALSE(admitted.empty());

    const pci::RasterTileData *tile = streamer.tile(admitted.front());
    REQUIRE(tile != nullptr);
    CHECK(tile->rgba.size() == pci::rasterStoredTileBytes);
}

TEST_CASE("a sparse BigTIFF costs its tiles, not its extent",
          "[component][raster][stress]")
{
    // 100000 squared, BigTIFF, sparse on disk and without overviews: the other
    // shape of "enormous", where the pixels are real rather than assembled
    // from members.
    const pci::GdalRasterLoader loader;
    const pci::RasterLayer layer =
        layerFor(load(fixtures().sparseHuge, loader));
    const pci::RasterLayerMetadata &metadata = layer.data->metadata();
    const auto *source =
        dynamic_cast<const pci::GdalRasterSource *>(layer.data->source.get());
    REQUIRE(source != nullptr);
    REQUIRE(metadata.width == 100000);
    CHECK(metadata.insufficientOverviews);

    const pci::Vec3d center{
        (metadata.bounds.minimum[0] + metadata.bounds.maximum[0]) * 0.5,
        (metadata.bounds.minimum[1] + metadata.bounds.maximum[1]) * 0.5,
        0.0};

    const std::uint64_t before = source->readCount();
    pci::RasterTileStreamer streamer(32ULL * 1024 * 1024, 2);
    const pci::RasterLodPlan plan =
        planRasterTiles(planInput(layer, overheadCamera(center, 20000.0)));
    streamer.reconcile(plan, layer);
    streamer.waitForIdle();
    static_cast<void>(streamer.drainCompletions({}));

    const pci::RasterStreamerMetrics metrics = streamer.metrics();
    // The decoded cache holds the line even though the plan wanted more than
    // it can fit, and the read count follows the requests rather than the
    // 1e10 pixels behind them.
    CHECK(metrics.cpuBytes <= 32ULL * 1024 * 1024);
    CHECK(source->readCount() - before ==
          metrics.requested * metadata.bands.size());
    CHECK(metrics.requested <= pci::rasterMaximumPendingRequests);
}

TEST_CASE("a huge mosaic covers coarsely before it refines",
          "[component][raster][stress]")
{
    const pci::GdalRasterLoader loader;
    const pci::RasterLayer layer = layerFor(load(fixtures().vrtMosaic, loader));
    const pci::RasterLayerMetadata &metadata = layer.data->metadata();
    REQUIRE(metadata.levels.size() == 3);

    const pci::Vec3d center{
        (metadata.bounds.minimum[0] + metadata.bounds.maximum[0]) * 0.5,
        (metadata.bounds.minimum[1] + metadata.bounds.maximum[1]) * 0.5,
        0.0};

    // Far away, the coarsest backed level is what the planner asks for, and
    // requests are ordered coarse-first so coverage arrives before detail.
    const pci::RasterLodPlan wide =
        planRasterTiles(planInput(layer, overheadCamera(center, 900000.0)));
    REQUIRE_FALSE(wide.requests.empty());
    const std::uint32_t coarsest =
        static_cast<std::uint32_t>(metadata.levels.size() - 1);
    CHECK(wide.requests.front().levelIndex == coarsest);
    CHECK(std::ranges::is_sorted(
        wide.requests,
        [](const pci::RasterTileKey left, const pci::RasterTileKey right) {
            return left.levelIndex > right.levelIndex;
        }));

    // Closer in, the same layer resolves finer without changing its bounds.
    const pci::RasterLodPlan near =
        planRasterTiles(planInput(layer, overheadCamera(center, 2000.0)));
    REQUIRE_FALSE(near.selected.empty());
    const std::uint32_t finest =
        std::ranges::min(near.selected, {}, &pci::RasterTileKey::levelIndex)
            .levelIndex;
    CHECK(finest < coarsest);
    CHECK(near.selected.size() <= 512);
}

} // namespace
