#include <pci/runtime/scene/SceneRuntime.h>

#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

class RuntimeRasterSource final : public pci::RasterTileSource {
public:
    RuntimeRasterSource()
    {
        metadata_.width = 1;
        metadata_.height = 1;
        metadata_.geoTransform = {0.0, 1.0, 0.0, 1.0, 0.0, -1.0};
        metadata_.bounds = *pci::rasterPixelEdgeBounds(
            metadata_.geoTransform, metadata_.width, metadata_.height);
        metadata_.levels.push_back(
            {.width = 1, .height = 1, .channelCount = 4});
    }

    [[nodiscard]] const pci::RasterLayerMetadata &
    metadata() const noexcept override
    {
        return metadata_;
    }

    [[nodiscard]] pci::RasterTileData readTile(const pci::RasterTileRequest &,
                                               std::stop_token) const override
    {
        throw pci::RasterReadError("unused tile read");
    }

private:
    pci::RasterLayerMetadata metadata_;
};

[[nodiscard]] pci::PointDatasetRuntimePtr pointScene()
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePath = "runtime.las";
    return std::make_shared<pci::PointDatasetRuntime>(std::move(metadata));
}

[[nodiscard]] pci::PointCloudNodePayloadPtr
runtimeRootPayload(const std::size_t retainedPointCount = 1,
                   const std::uint64_t sourcePointCount = 8)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(retainedPointCount);
    block->attributes.resize(retainedPointCount);
    auto payload = std::make_shared<pci::PointCloudNodePayload>();
    payload->nodeId = pci::rootPointCloudNode;
    payload->sourcePointCount = sourcePointCount;
    payload->blocks.push_back(std::move(block));
    return payload;
}

class RuntimeGatedHierarchySource final : public pci::PointCloudDataSource {
public:
    explicit RuntimeGatedHierarchySource(
        const bool waitsForRelease,
        pci::Bounds3d bounds = {.minimum = {0.0, 0.0, 0.0},
                                .maximum = {2.0, 2.0, 2.0}})
        : waitsForRelease_(waitsForRelease)
        , bounds_(bounds)
    {
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        return {
            .id = id,
            .bounds = pci::pointCloudNodeBounds(bounds_, id),
            .geometricError = id.level == 0 ? 1.0 : 0.0,
            .estimatedPointCount = 4,
            .leaf = id.level != 0,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId id,
             const std::stop_token stopToken) const override
    {
        started = true;
        while (waitsForRelease_ && !released && !stopToken.stop_requested()) {
            std::this_thread::yield();
        }
        if (stopToken.stop_requested()) {
            throw pci::PointCloudDataSourceCancelled();
        }
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(4);
        block->attributes.resize(4);
        auto payload = std::make_shared<pci::PointCloudNodePayload>();
        payload->nodeId = id;
        payload->sourcePointCount = 4;
        payload->blocks.push_back(std::move(block));
        return payload;
    }

    mutable std::atomic_bool started = false;
    mutable std::atomic_bool released = false;

private:
    bool waitsForRelease_ = false;
    pci::Bounds3d bounds_;
};

[[nodiscard]] pci::PointDatasetRuntimePtr
hierarchicalSceneWith(const pci::Bounds3d bounds,
                      const std::size_t rootPointCount,
                      const std::uint64_t standaloneBudget)
{
    pci::PointCloudMetadata metadata;
    metadata.sourceBounds = bounds;
    metadata.sourcePointCount = rootPointCount;
    return std::make_shared<pci::PointDatasetRuntime>(
        std::move(metadata),
        std::make_shared<RuntimeGatedHierarchySource>(false, bounds),
        runtimeRootPayload(rootPointCount, rootPointCount),
        standaloneBudget);
}

void attachPoint(pci::SceneRuntime &runtime,
                 const pci::PointDatasetRuntimePtr &scene,
                 const pci::BindingGeneration generation,
                 const bool active = true)
{
    if (!runtime.attachPoint({.descriptor = scene->descriptor(),
                              .runtime = scene,
                              .generation = generation,
                              .active = active})) {
        throw std::logic_error("point runtime attachment failed");
    }
}

TEST_CASE(
    "prepared cache root changes preserve unrelated concurrent insertions",
    "[unit][scene-runtime][transaction][concurrency]")
{
    pci::DecodedPageCache cache(1024 * 1024);
    const pci::PointCloudSourceId first{1};
    const pci::PointCloudSourceId other{2};
    auto original = runtimeRootPayload();
    auto replacement = runtimeRootPayload(2);
    auto unrelated = runtimeRootPayload(3);
    cache.insert(first, original);
    const std::array pins{pci::rootPointCloudNode};
    auto prepared = cache.prepareSourceRoot(first, replacement, pins);
    std::latch started(1);
    std::jthread writer([&] {
        started.count_down();
        cache.insert(other, unrelated);
    });
    started.wait();
    prepared->commit();
    writer.join();
    CHECK(cache.peek(first, pci::rootPointCloudNode) == replacement);
    CHECK(cache.peek(other, pci::rootPointCloudNode) == unrelated);
    const auto before = cache.metrics();
    auto abandoned = cache.prepareSourceRoot(first, original, pins);
    abandoned.reset();
    CHECK(cache.peek(first, pci::rootPointCloudNode) == replacement);
    CHECK(cache.peek(other, pci::rootPointCloudNode) == unrelated);
    CHECK(cache.metrics().residentBytes == before.residentBytes);
    CHECK(cache.metrics().insertions == before.insertions);
}

TEST_CASE(
    "overlay registry candidates share point residency without reattachment",
    "[unit][scene-runtime][transaction]")
{
    pci::SceneRuntime runtime;
    auto source = std::make_shared<RuntimeGatedHierarchySource>(true);
    auto point = std::make_shared<pci::PointDatasetRuntime>(
        pci::PointCloudMetadata{.sourcePointCount = 8},
        source,
        runtimeRootPayload(),
        1024 * 1024);
    REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                 .runtime = point,
                                 .generation = pci::BindingGeneration{1}}));
    const auto revision = point->revision();
    const auto cache = runtime.decodedPageCache();
    const auto scheduler = runtime.hierarchyScheduler();
    const std::array requested{
        pci::PointCloudNodeId{.level = 1, .x = 0, .y = 0, .z = 0}};
    point->requestNodes(requested);
    auto registry = runtime.copyBindings();
    auto raster = std::make_shared<RuntimeRasterSource>();
    pci::RasterDatasetDescriptor descriptor{.sourceId = pci::RasterSourceId{1},
                                            .metadata = raster->metadata()};
    REQUIRE(registry.attachRaster({.descriptor = descriptor,
                                   .source = raster,
                                   .generation = pci::BindingGeneration{2}}));
    static_cast<void>(registry.snapshot());
    runtime.swap(registry);
    CHECK(runtime.decodedPageCache() == cache);
    CHECK(runtime.hierarchyScheduler() == scheduler);
    CHECK(point->revision() == revision);
    auto removal = runtime.copyBindings();
    REQUIRE(removal.stageDetach(pci::BindingGeneration{2}));
    runtime.swap(removal);
    CHECK(runtime.decodedPageCache() == cache);
    CHECK(point->revision() == revision);
    source->released = true;
    scheduler->waitForIdle();
    CHECK_FALSE(point->peekNodePayload(requested.front()));
    REQUIRE(point->drainDecodeCompletions() == 1);
    CHECK(point->peekNodePayload(requested.front()));
    CHECK(point->hierarchyMetrics().decodeRequestsCancelled == 0);
}

} // namespace

TEST_CASE("scene runtime resolves bindings only through matching strong "
          "identities",
          "[scene][runtime][binding][strong-id]")
{
    pci::SceneRuntime runtime;
    const auto point = pointScene();
    const auto raster = std::make_shared<RuntimeRasterSource>();
    const pci::RasterSourceId rasterId{91};

    REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                 .runtime = point,
                                 .generation = pci::BindingGeneration{7}}));
    REQUIRE(runtime.attachRaster(
        {.descriptor = {.sourceId = rasterId, .metadata = raster->metadata()},
         .source = raster,
         .generation = pci::BindingGeneration{8}}));

    CHECK(runtime.bindingCount() == 2);
    CHECK(runtime.pointBindingCount() == 1);
    CHECK(runtime.rasterBindingCount() == 1);
    CHECK(runtime.point(point->sourceId(), pci::BindingGeneration{7}) == point);
    CHECK_FALSE(runtime.point(point->sourceId(), pci::BindingGeneration{8}));
    CHECK_FALSE(
        runtime.point(pci::PointCloudSourceId{999}, pci::BindingGeneration{7}));
    CHECK(runtime.raster(rasterId, pci::BindingGeneration{8}) == raster);
    CHECK_FALSE(runtime.raster(rasterId, pci::BindingGeneration{7}));
    CHECK_FALSE(
        runtime.raster(pci::RasterSourceId{999}, pci::BindingGeneration{8}));
}

TEST_CASE("scene runtime rejects duplicate and malformed attachments without "
          "changing active bindings",
          "[scene][runtime][binding][exceptions]")
{
    pci::SceneRuntime runtime;
    const auto point = pointScene();
    REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                 .runtime = point,
                                 .generation = pci::BindingGeneration{3}}));

    const auto duplicateSource = pointScene();
    CHECK_FALSE(runtime.attachPoint({.descriptor = point->descriptor(),
                                     .runtime = point,
                                     .generation = pci::BindingGeneration{4}}));
    CHECK_FALSE(
        runtime.attachPoint({.descriptor = duplicateSource->descriptor(),
                             .runtime = duplicateSource,
                             .generation = pci::BindingGeneration{3}}));
    CHECK_THROWS_AS(
        runtime.attachPoint({.descriptor = point->descriptor(),
                             .runtime = {},
                             .generation = pci::BindingGeneration{5}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        runtime.attachPoint({.descriptor = duplicateSource->descriptor(),
                             .runtime = point,
                             .generation = pci::BindingGeneration{5}}),
        std::invalid_argument);

    CHECK(runtime.bindingCount() == 1);
    CHECK(runtime.detach(pci::BindingGeneration{3}));
    CHECK_FALSE(runtime.detach(pci::BindingGeneration{3}));
    CHECK(runtime.bindingCount() == 0);
}

TEST_CASE("scene runtime clear releases point and raster bindings",
          "[scene][runtime][binding][lifecycle]")
{
    pci::SceneRuntime runtime;
    const auto point = pointScene();
    const auto raster = std::make_shared<RuntimeRasterSource>();
    REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                 .runtime = point,
                                 .generation = pci::BindingGeneration{1}}));
    REQUIRE(
        runtime.attachRaster({.descriptor = {.sourceId = pci::RasterSourceId{2},
                                             .metadata = raster->metadata()},
                              .source = raster,
                              .generation = pci::BindingGeneration{2}}));

    runtime.clear();

    CHECK(runtime.bindingCount() == 0);
    CHECK_FALSE(runtime.point(point->sourceId(), pci::BindingGeneration{1}));
    CHECK_FALSE(
        runtime.raster(pci::RasterSourceId{2}, pci::BindingGeneration{2}));
}

TEST_CASE("scene runtime snapshots are cached and remain immutable after "
          "binding changes",
          "[scene][runtime][binding][snapshot]")
{
    pci::SceneRuntime runtime;
    const auto point = pointScene();
    REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                 .runtime = point,
                                 .generation = pci::BindingGeneration{4}}));

    const auto attached = runtime.snapshot();
    CHECK(runtime.snapshot() == attached);
    REQUIRE(attached);
    CHECK(attached->pointBindingCount() == 1);
    CHECK(attached->point(point->sourceId(), pci::BindingGeneration{4}) ==
          point);

    REQUIRE(runtime.detach(pci::BindingGeneration{4}));
    const auto detached = runtime.snapshot();
    CHECK(detached != attached);
    CHECK(detached->bindingCount() == 0);
    CHECK(attached->pointBindingCount() == 1);
    CHECK(attached->point(point->sourceId(), pci::BindingGeneration{4}) ==
          point);
}

TEST_CASE("scene runtime validates point activity batches before mutation",
          "[scene][runtime][binding][activity]")
{
    pci::SceneRuntime runtime;
    const auto first = pointScene();
    const auto second = pointScene();
    REQUIRE(runtime.attachPoint({.descriptor = first->descriptor(),
                                 .runtime = first,
                                 .generation = pci::BindingGeneration{1}}));
    REQUIRE(runtime.attachPoint({.descriptor = second->descriptor(),
                                 .runtime = second,
                                 .generation = pci::BindingGeneration{2}}));
    const auto initial = runtime.snapshot();
    REQUIRE(initial->pointActive(first->sourceId(),
                                 pci::BindingGeneration{1}) == true);
    REQUIRE(initial->pointActive(second->sourceId(),
                                 pci::BindingGeneration{2}) == true);

    std::array invalid{
        pci::PointRuntimeActivityUpdate{.sourceId = first->sourceId(),
                                        .generation = pci::BindingGeneration{1},
                                        .active = false},
        pci::PointRuntimeActivityUpdate{.sourceId =
                                            pci::PointCloudSourceId{999},
                                        .generation = pci::BindingGeneration{2},
                                        .active = false},
    };
    CHECK_FALSE(runtime.setPointActive(invalid));
    CHECK(runtime.snapshot() == initial);
    CHECK(runtime.snapshot()->pointActive(first->sourceId(),
                                          pci::BindingGeneration{1}) == true);

    invalid[1] = invalid[0];
    CHECK_FALSE(runtime.setPointActive(invalid));
    CHECK(runtime.snapshot() == initial);

    const std::array updates{
        invalid[0],
        pci::PointRuntimeActivityUpdate{.sourceId = second->sourceId(),
                                        .generation = pci::BindingGeneration{2},
                                        .active = false},
    };
    REQUIRE(runtime.setPointActive(updates));
    const auto updated = runtime.snapshot();
    CHECK(updated != initial);
    CHECK(updated->pointActive(first->sourceId(), pci::BindingGeneration{1}) ==
          false);
    CHECK(updated->pointActive(second->sourceId(), pci::BindingGeneration{2}) ==
          false);
    CHECK(initial->pointActive(first->sourceId(), pci::BindingGeneration{1}) ==
          true);
}

TEST_CASE("runtime bounds decode concurrency across hierarchical layers",
          "[unit][scene][hierarchy][residency][concurrency]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto firstSource =
        std::make_shared<RuntimeGatedHierarchySource>(true);
    const auto secondSource =
        std::make_shared<RuntimeGatedHierarchySource>(false);
    auto firstScene = std::make_shared<pci::PointDatasetRuntime>(
        metadata, firstSource, runtimeRootPayload(), 1024 * 1024);
    auto secondScene = std::make_shared<pci::PointDatasetRuntime>(
        metadata, secondSource, runtimeRootPayload(), 1024 * 1024);
    pci::SceneRuntime runtime(1024 * 1024, 1);
    REQUIRE(runtime.attachPoint({.descriptor = firstScene->descriptor(),
                                 .runtime = firstScene,
                                 .generation = pci::BindingGeneration{1}}));
    REQUIRE(runtime.attachPoint({.descriptor = secondScene->descriptor(),
                                 .runtime = secondScene,
                                 .generation = pci::BindingGeneration{2}}));
    const pci::PointCloudNodeId child =
        pci::childNodeId(pci::rootPointCloudNode, 0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);

    firstScene->requestNodes(std::array{child});
    while (!firstSource->started &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(firstSource->started);
    secondScene->requestNodes(std::array{child});
    while (runtime.hierarchyScheduler()->metrics().pending == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    CHECK(runtime.hierarchyScheduler()->metrics().active == 1);
    REQUIRE(runtime.hierarchyScheduler()->metrics().pending == 1);
    CHECK(runtime.residencyCoordinator()->activeDecodeCount() == 0);
    CHECK(runtime.residencyCoordinator()->pendingDecodeCount() == 0);
    CHECK_FALSE(secondSource->started);

    firstSource->released = true;
    pci::PointCloudNodePayloadPtr secondPayload;
    while (!secondPayload && std::chrono::steady_clock::now() < deadline) {
        secondScene->drainDecodeCompletions();
        secondPayload = secondScene->nodePayload(child);
        std::this_thread::yield();
    }
    REQUIRE(secondPayload);
    CHECK(secondSource->started);
}

TEST_CASE("runtime binding churn releases shared cache entries",
          "[unit][scene][hierarchy][residency][churn]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    constexpr std::size_t layerCount = 25;
    pci::SceneRuntime runtime(1024 * 1024, 2);
    std::vector<pci::PointDatasetRuntimePtr> scenes;
    scenes.reserve(layerCount);
    for (std::size_t index = 0; index < layerCount; ++index) {
        auto scene = hierarchicalSceneWith(bounds, 1, 4096);
        attachPoint(
            runtime,
            scene,
            pci::BindingGeneration{static_cast<std::uint64_t>(index + 1)});
        scenes.push_back(std::move(scene));
    }
    CHECK(runtime.decodedPageCache()->size() == layerCount);

    for (std::size_t index = 0; index < scenes.size(); index += 2) {
        REQUIRE(runtime.setPointActive(
            scenes[index]->sourceId(),
            pci::BindingGeneration{static_cast<std::uint64_t>(index + 1)},
            false));
    }
    for (std::size_t index = 0; index < scenes.size(); index += 2) {
        REQUIRE(runtime.setPointActive(
            scenes[index]->sourceId(),
            pci::BindingGeneration{static_cast<std::uint64_t>(index + 1)},
            true));
    }
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        REQUIRE(runtime.detach(
            pci::BindingGeneration{static_cast<std::uint64_t>(index + 1)}));
    }
    CHECK(runtime.bindingCount() == 0);
    CHECK(runtime.decodedPageCache()->size() == 0);
    CHECK(runtime.decodedResidentBytes() == 0);
}

TEST_CASE("runtime refuses roots that exceed its shared CPU budget",
          "[unit][scene][hierarchy][residency][admission]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto first = hierarchicalSceneWith(bounds, 1, 4096);
    auto second = hierarchicalSceneWith(bounds, 1, 4096);
    const std::uint64_t oneRoot = first->decodedResidentBytes();
    pci::SceneRuntime runtime(oneRoot, 1);
    attachPoint(runtime, first, pci::BindingGeneration{1});
    CHECK_THROWS_AS(
        runtime.attachPoint({.descriptor = second->descriptor(),
                             .runtime = second,
                             .generation = pci::BindingGeneration{2}}),
        std::length_error);
    CHECK(runtime.bindingCount() == 1);
    CHECK(runtime.decodedResidentBytes() == oneRoot);
}

TEST_CASE("runtime thins root previews evenly before refusing a source",
          "[unit][scene][hierarchy][residency][fairness]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto first = hierarchicalSceneWith(bounds, 4, 4096);
    auto second = hierarchicalSceneWith(bounds, 4, 4096);
    const std::uint64_t oneFullRoot = first->decodedResidentBytes();
    pci::SceneRuntime runtime(oneFullRoot, 1);
    attachPoint(runtime, first, pci::BindingGeneration{1});
    attachPoint(runtime, second, pci::BindingGeneration{2});

    CHECK(runtime.bindingCount() == 2);
    CHECK(first->decodedResidentPoints() == 2);
    CHECK(second->decodedResidentPoints() == 2);
    CHECK(runtime.decodedResidentBytes() == oneFullRoot);
    CHECK(runtime.decodedResidentBytes() <= runtime.decodedByteBudget());
}

TEST_CASE("runtime budget synchronization cannot strand pinned roots",
          "[unit][scene][hierarchy][residency][memory]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto scene = hierarchicalSceneWith(bounds, 4, 4096);
    const std::uint64_t rootBytes = scene->decodedResidentBytes();
    pci::SceneRuntime runtime(rootBytes + 100, 1);
    attachPoint(runtime, scene, pci::BindingGeneration{1});

    REQUIRE(runtime.memoryBudget()->setByteBudget(rootBytes - 1));
    runtime.syncResidencyBudgets();
    CHECK(runtime.memoryBudget()->byteBudget() == rootBytes - 1);
    CHECK(runtime.decodedPageCache()->byteBudget() == rootBytes - 1);
    CHECK(runtime.decodedResidentBytes() < rootBytes);
    CHECK(runtime.decodedResidentBytes() <=
          runtime.decodedPageCache()->byteBudget());
}

TEST_CASE("runtime owns one decoded cache shared by hierarchical layers",
          "[unit][scene][hierarchy][residency]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto firstScene = hierarchicalSceneWith(bounds, 1, 4096);
    auto secondScene = hierarchicalSceneWith(bounds, 1, 4096);
    const std::uint64_t retainedRoots = firstScene->decodedResidentBytes() +
                                        secondScene->decodedResidentBytes();
    const std::uint64_t documentBudget = retainedRoots + 2000;
    pci::SceneRuntime runtime(documentBudget, 1);

    attachPoint(runtime, firstScene, pci::BindingGeneration{1});
    attachPoint(runtime, secondScene, pci::BindingGeneration{2});

    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(secondScene->decodedByteBudget() == documentBudget);
    CHECK(runtime.decodedPageCache()->byteBudget() == documentBudget);
    CHECK(runtime.decodedResidentBytes() == retainedRoots);
    REQUIRE(firstScene->nodePayload(pci::rootPointCloudNode));
    const pci::SceneRuntimeMetrics initialMetrics = runtime.metrics();
    CHECK(initialMetrics.hierarchicalLayers == 2);
    CHECK(initialMetrics.cache.hits == 1);
    CHECK(initialMetrics.cache.insertions == 2);
    CHECK(initialMetrics.cache.residentBytes == retainedRoots);
    CHECK(initialMetrics.cache.byteBudget == documentBudget);
    CHECK(initialMetrics.cache.peakResidentBytes == retainedRoots);

    REQUIRE(runtime.setPointActive(
        secondScene->sourceId(), pci::BindingGeneration{2}, false));
    CHECK(secondScene->decodedByteBudget() == documentBudget);
    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(runtime.decodedPageCache()->residentBytes() == retainedRoots);

    REQUIRE(runtime.detach(pci::BindingGeneration{2}));
    CHECK(firstScene->decodedByteBudget() == documentBudget);
    CHECK(runtime.decodedPageCache()->residentBytes() ==
          firstScene->decodedResidentBytes());
    CHECK(runtime.point(firstScene->sourceId(), pci::BindingGeneration{1}) ==
          firstScene);
}

TEST_CASE("hierarchical scenes transition from standalone to shared runtime "
          "residency and back",
          "[unit][scene][hierarchy][residency][lifecycle]")
{
    const pci::Bounds3d bounds{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    constexpr std::uint64_t standaloneBudget = 4096;
    constexpr std::uint64_t documentBudget = 8192;
    auto scene = hierarchicalSceneWith(bounds, 1, standaloneBudget);

    CHECK(scene->decodedByteBudget() == standaloneBudget);
    {
        pci::SceneRuntime runtime(documentBudget, 1);
        attachPoint(runtime, scene, pci::BindingGeneration{1});
        CHECK(scene->decodedByteBudget() == documentBudget);

        REQUIRE(runtime.detach(pci::BindingGeneration{1}));
        CHECK(scene->decodedByteBudget() == standaloneBudget);
    }

    scene.reset();
}

TEST_CASE(
    "discarded hierarchy attachment candidates preserve active roots and cache",
    "[scene][runtime][transaction]")
{
    const auto root = runtimeRootPayload(8);
    const auto bytes = pci::pointCloudNodePayloadBytes(*root);
    pci::PointCloudMetadata metadata;
    auto scene = std::make_shared<pci::PointDatasetRuntime>(
        metadata,
        std::make_shared<RuntimeGatedHierarchySource>(false),
        root,
        bytes * 4);
    pci::SceneRuntime active(bytes * 4, 1);
    attachPoint(active, scene, pci::BindingGeneration{1});
    const auto revision = scene->revision();
    const auto activeCache = active.decodedPageCache();
    const auto budget = scene->decodedByteBudget();
    {
        pci::SceneRuntime candidate(bytes / 2, 1, {}, {}, activeCache->clone());
        REQUIRE(
            candidate.stagePoint({.descriptor = scene->descriptor(),
                                  .runtime = scene,
                                  .generation = pci::BindingGeneration{2}}));
        candidate.preparePointAttachments();
        CHECK(candidate.decodedPageCache()->peek(
                  scene->sourceId(), pci::rootPointCloudNode) != root);
        CHECK(scene->peekNodePayload(pci::rootPointCloudNode) == root);
        CHECK(scene->revision() == revision);
        CHECK(scene->decodedByteBudget() == budget);
    }
    CHECK(activeCache->peek(scene->sourceId(), pci::rootPointCloudNode) ==
          root);
    CHECK(scene->peekNodePayload(pci::rootPointCloudNode) == root);
    CHECK(scene->revision() == revision);
}

TEST_CASE("reserved roots are counted once and excluded rollback roots remain "
          "charged",
          "[scene][runtime][memory][rollback]")
{
    const auto root = runtimeRootPayload(8);
    const auto bytes = pci::pointCloudNodePayloadBytes(*root);
    auto memory = std::make_shared<pci::PointMemoryBudget>(bytes * 3);
    auto reservation = memory->tryReserve(bytes);
    REQUIRE(reservation);
    auto old = std::make_shared<pci::PointDatasetRuntime>(
        pci::PointCloudMetadata{},
        std::make_shared<RuntimeGatedHierarchySource>(false),
        root,
        bytes * 3,
        true,
        pci::allocatePointCloudSourceId(),
        *reservation);
    reservation.reset();
    pci::SceneRuntime active(bytes * 3, 1, {}, memory);
    REQUIRE(active.stagePoint({.descriptor = old->descriptor(),
                               .runtime = old,
                               .generation = pci::BindingGeneration{1}}));
    active.preparePointAttachments();
    active.commitPointAttachments();
    CHECK(memory->reservedBytes() == bytes);
    CHECK(active.residencyCoordinator()->availableResidencyBytes() ==
          bytes * 3);
    CHECK(active.residencyCoordinator()->retainedRootBytes() == 0);
    {
        const auto rollback = active.snapshot();
        pci::SceneRuntime replacement(bytes * 3, 1, {}, memory);
        replacement.preparePointAttachments();
        replacement.commitPointAttachments();
        active.swap(replacement);
        CHECK(active.residencyCoordinator()->availableResidencyBytes() ==
              bytes * 2);
        CHECK(memory->reservedBytes() == bytes);
        CHECK(rollback->point(old->sourceId(), pci::BindingGeneration{1}) ==
              old);
    }
    old.reset();
    CHECK(memory->reservedBytes() == 0);
}
