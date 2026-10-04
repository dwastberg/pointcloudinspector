#include <pci/document/SceneDocument.h>
#include <pci/rendering/SceneSnapshotCache.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

pci::PointBlockPtr onePointBlock(const double x)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->points.push_back({});
    block->bounds = {
        .minimum = {x, 0.0, 0.0},
        .maximum = {x, 0.0, 0.0},
    };
    return block;
}

struct PointDocument {
    pci::SceneDocumentSnapshotPtr snapshot;
    pci::SceneRuntimeSnapshotPtr runtime;
    pci::PointDatasetRuntimePtr scene;
    pci::PointCloudLayerId layerId;
};

PointDocument pointDocument(const double x)
{
    auto scene =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    scene->addBlock(onePointBlock(x));
    auto document = std::make_shared<pci::SceneDocument>();
    const pci::PointCloudLayerId layerId =
        document->addLayer(scene->datasetView());
    const auto layer = document->layer(layerId);
    REQUIRE(layer);
    pci::SceneRuntime runtime;
    REQUIRE(runtime.attachPoint({.descriptor = layer->descriptor,
                                 .runtime = scene,
                                 .generation = layer->bindingGeneration}));
    return {
        .snapshot = document->snapshot(),
        .runtime = runtime.snapshot(),
        .scene = std::move(scene),
        .layerId = layerId,
    };
}

TEST_CASE("scene snapshot cache reconciles membership and stable layer ids",
          "[unit][renderer-internal][snapshot-cache]")
{
    pci::SceneSnapshotCache cache;
    PointDocument first = pointDocument(1.0);
    pci::SceneRuntime missingRuntime;
    CHECK_THROWS_AS(
        cache.setDocument(first.snapshot, missingRuntime.snapshot(), true),
        std::invalid_argument);
    cache.setDocument(first.snapshot, first.runtime, true);

    CHECK_FALSE(cache.snapshot(first.layerId));
    CHECK(cache.refresh().invalidatedRootPayloads.empty());
    REQUIRE(cache.snapshot(first.layerId));
    CHECK(cache.snapshot(first.layerId)->revision == 1);

    const std::array visible{first.layerId};
    const auto added = cache.reconcileVisibleLayers(visible);
    CHECK(added.added == std::vector{first.layerId});
    CHECK(added.removed.empty());
    CHECK(cache.containsVisibleLayer(first.layerId));
    const auto unchanged = cache.reconcileVisibleLayers(visible);
    CHECK(unchanged.added.empty());
    CHECK(unchanged.removed.empty());

    auto emptyDocument = std::make_shared<pci::SceneDocument>();
    pci::SceneRuntime emptyRuntime;
    cache.setDocument(
        emptyDocument->snapshot(), emptyRuntime.snapshot(), false);
    CHECK(cache.refresh().invalidatedRootPayloads.empty());
    CHECK_FALSE(cache.snapshot(first.layerId));
    const auto removed =
        cache.reconcileVisibleLayers(std::span<const pci::PointCloudLayerId>{});
    CHECK(removed.added.empty());
    CHECK(removed.removed == std::vector{first.layerId});
}

TEST_CASE("scene snapshot cache coalesces wakes and ignores stale callbacks",
          "[unit][renderer-internal][snapshot-cache][invalidation]")
{
    std::vector<std::function<void()>> dispatched;
    std::uint64_t wakes = 0;
    pci::SceneSnapshotCache cache;
    cache.setCallbacks(
        [&wakes] {
            ++wakes;
        },
        [&dispatched](std::function<void()> callback) {
            dispatched.push_back(std::move(callback));
            return true;
        });

    PointDocument first = pointDocument(1.0);
    cache.setDocument(first.snapshot, first.runtime, true);
    static_cast<void>(cache.refresh());
    first.scene->addBlock(onePointBlock(2.0));
    first.scene->addBlock(onePointBlock(3.0));
    REQUIRE(dispatched.size() == 1);
    dispatched.back()();
    CHECK(wakes == 1);

    static_cast<void>(cache.refresh());
    CHECK(cache.snapshot(first.layerId)->revision == 3);
    first.scene->addBlock(onePointBlock(4.0));
    REQUIRE(dispatched.size() == 2);

    // Replace the subscribed scene before its already queued callback runs.
    // The delivery rechecks current cache membership and must not wake for the
    // retired scene.
    PointDocument replacement = pointDocument(10.0);
    REQUIRE(replacement.layerId == first.layerId);
    cache.setDocument(replacement.snapshot, replacement.runtime, false);
    dispatched.back()();
    CHECK(wakes == 1);

    static_cast<void>(cache.refresh());
    replacement.scene->addBlock(onePointBlock(11.0));
    REQUIRE(dispatched.size() == 3);
    dispatched.back()();
    CHECK(wakes == 2);
}

TEST_CASE("scene snapshot cache retries a rejected wake dispatch",
          "[unit][renderer-internal][snapshot-cache][invalidation]")
{
    std::uint64_t dispatchAttempts = 0;
    pci::SceneSnapshotCache cache;
    cache.setCallbacks([] {},
                       [&dispatchAttempts](std::function<void()>) {
                           ++dispatchAttempts;
                           return false;
                       });
    PointDocument document = pointDocument(1.0);
    cache.setDocument(document.snapshot, document.runtime, true);
    static_cast<void>(cache.refresh());

    document.scene->addBlock(onePointBlock(2.0));
    document.scene->addBlock(onePointBlock(3.0));
    CHECK(dispatchAttempts == 2);
}

TEST_CASE("scene snapshot cache reports color-generation invalidation",
          "[unit][renderer-internal][snapshot-cache][colorize]")
{
    auto scene =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    scene->addBlock(onePointBlock(1.0));
    auto document = std::make_shared<pci::SceneDocument>();
    const pci::PointCloudLayerId layerId =
        document->addLayer(scene->datasetView());
    const auto layer = document->layer(layerId);
    REQUIRE(layer);
    pci::SceneRuntime runtime;
    REQUIRE(runtime.attachPoint({.descriptor = layer->descriptor,
                                 .runtime = scene,
                                 .generation = layer->bindingGeneration}));

    pci::SceneSnapshotCache cache;
    cache.setDocument(document->snapshot(), runtime.snapshot(), true);
    static_cast<void>(cache.refresh());
    REQUIRE(cache.snapshot(layerId));

    CHECK(document->setLayerRasterColors(
        layerId,
        {.rasterSourceId = pci::nextRasterSourceId(),
         .rasterSourcePath = "colors.tif",
         .decode = std::make_shared<pci::RasterDecodeParameters>()}));
    cache.setDocument(document->snapshot(), runtime.snapshot(), false);
    const pci::SceneSnapshotCache::RefreshResult changed = cache.refresh();
    CHECK(changed.invalidatedColors == std::vector{layerId});
    CHECK(changed.invalidatedRootPayloads.empty());

    cache.setDocument(document->snapshot(), runtime.snapshot(), false);
    CHECK(cache.refresh().invalidatedColors.empty());
}

} // namespace
