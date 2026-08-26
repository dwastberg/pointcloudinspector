#include "fixtures/GdalRasterFixtureFactory.h"
#include "import/gdal/GdalRasterLoader.h"
#include "import/gdal/GdalRasterSource.h"
#include "import/gdal/GdalRuntime.h"
#include "platform/ProcessMemory.h"
#include "renderer/planning/RasterLodPlanner.h"
#include "renderer/rhi/RasterTileStreamer.h"
#include "storage/SecureStorage.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

// The corpus is fixed in size rather than scaled to available memory. A
// memory-relative fixture behaves differently on CI than on a workstation and
// turns a pass/fail bound into a coin flip.
struct GdalFixtures final {
    GdalFixtures()
        : directory(std::filesystem::temp_directory_path(), "pci-raster-stress")
        , paths(pci::test::writeGdalRasterFixtures(directory.path()))
    {
    }

    pci::PrivateTemporaryDirectory directory;
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
    constexpr std::uint64_t cpuBudget = 64ULL * 1024 * 1024;
    constexpr std::uint64_t gdalBudget = 32ULL * 1024 * 1024;
    constexpr std::uint64_t allocatorAllowance = 256ULL * 1024 * 1024;

    // Establish the third allocator's exact limit before inspection or tile
    // reads, then take RSS only after fixture creation and GDAL registration.
    pci::setGdalBlockCacheBytes(gdalBudget);
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

    pci::RasterTileStreamer streamer(cpuBudget, 2);
    const std::uint64_t rssBaseline = pci::processMemoryMetrics().residentBytes;
    const std::uint64_t readsBeforePlanning = source->readCount();

    // Whole-catalog view. The external overview backs a cheap generated root,
    // so requests stay small instead of falling through to base pixels.
    const pci::RasterLodPlan wide =
        planRasterTiles(planInput(layer, overheadCamera(center, 900000.0)));
    REQUIRE_FALSE(wide.selected.empty());
    CHECK(wide.selected.size() <= 512);
    CHECK_FALSE(wide.capacityLimited);
    CHECK_FALSE(wide.coverageIncomplete);
    const std::uint32_t coarsest =
        static_cast<std::uint32_t>(metadata.levels.size() - 1);
    CHECK(std::ranges::all_of(wide.selected, [](const pci::RasterTileKey key) {
        return key.levelIndex > 0;
    }));
    REQUIRE_FALSE(wide.requests.empty());
    CHECK(wide.requests.front().levelIndex == coarsest);
    // Planning reads nothing; it is arithmetic over the level table.
    CHECK(source->readCount() == readsBeforePlanning);

    streamer.reconcile(wide, layer);
    streamer.waitForIdle();
    static_cast<void>(
        streamer.drainCompletions({}, pci::rasterMaximumPendingRequests));
    static_cast<void>(
        streamer.takeReadyUploads(pci::rasterMaximumPendingRequests));

    pci::RasterStreamerMetrics metrics = streamer.metrics();
    CHECK(metrics.cpuBytes <= cpuBudget);
    CHECK(metrics.requested == wide.requests.size());
    CHECK(metrics.completed > 0);
    // One window per band per requested tile, and nothing else. This is the
    // assertion that fails if anything starts scanning the catalog: it is an
    // absolute count, unrelated to the source's 4e10 pixels.
    CHECK(source->readCount() - readsBeforePlanning ==
          metrics.requested * metadata.bands.size());

    CHECK(pci::gdalBlockCacheUsedBytes() <= gdalBudget);

    // Churn between disjoint catalog regions after the cache is already full.
    // Source size must not leak into the request queues or resident envelope,
    // and the allocator high-water should settle rather than grow each pass.
    const double width =
        metadata.bounds.maximum[0] - metadata.bounds.minimum[0];
    const double height =
        metadata.bounds.maximum[1] - metadata.bounds.minimum[1];
    constexpr std::array<double, 4> targetFractions{0.12, 0.38, 0.62, 0.88};
    std::array<pci::Vec3d, 16> targets{};
    std::size_t targetIndex = 0;
    for (const double y : targetFractions) {
        for (const double x : targetFractions) {
            targets[targetIndex++] = {
                metadata.bounds.minimum[0] + width * x,
                metadata.bounds.minimum[1] + height * y,
                0.0,
            };
        }
    }
    std::vector<std::uint64_t> residentSamples;
    residentSamples.reserve(targets.size());
    for (std::size_t pass = 0; pass < targets.size(); ++pass) {
        const pci::RasterLodPlan churn = planRasterTiles(planInput(
            layer, overheadCamera(targets[pass % targets.size()], 1500.0)));
        CAPTURE(pass);
        REQUIRE_FALSE(churn.selected.empty());
        CHECK(churn.selected.size() <= 512);

        streamer.reconcile(churn, layer);
        streamer.waitForIdle();
        static_cast<void>(
            streamer.drainCompletions({}, pci::rasterMaximumPendingRequests));
        static_cast<void>(
            streamer.takeReadyUploads(pci::rasterMaximumPendingRequests));

        metrics = streamer.metrics();
        CHECK(metrics.cpuBytes <= cpuBudget);
        CHECK(metrics.cpuPeakBytes <= cpuBudget);
        CHECK(metrics.pending == 0);
        CHECK(metrics.queued == 0);
        CHECK(metrics.pendingUploads == 0);
        CHECK(pci::gdalBlockCacheUsedBytes() <= gdalBudget);

        const std::uint64_t resident =
            pci::processMemoryMetrics().residentBytes;
        residentSamples.push_back(resident);
        if (rssBaseline > 0 && resident > rssBaseline) {
            CHECK(resident - rssBaseline <=
                  cpuBudget + gdalBudget + allocatorAllowance);
        }
    }

    REQUIRE(residentSamples.size() == targets.size());
    const auto warmBegin = residentSamples.end() - 4;
    const auto [warmMinimum, warmMaximum] =
        std::minmax_element(warmBegin, residentSamples.end());
    CHECK(*warmMaximum - *warmMinimum <= 64ULL * 1024 * 1024);
    CHECK(metrics.cacheEvictions > 0);
    CHECK(source->readCount() - readsBeforePlanning ==
          metrics.requested * metadata.bands.size());
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
    REQUIRE(pci::rasterBackedLevelCount(metadata.levels) == 3);

    const pci::Vec3d center{
        (metadata.bounds.minimum[0] + metadata.bounds.maximum[0]) * 0.5,
        (metadata.bounds.minimum[1] + metadata.bounds.maximum[1]) * 0.5,
        0.0};

    // Far away, the automatic root is what the planner asks for, and requests
    // are ordered coarse-first so coverage arrives before detail.
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
