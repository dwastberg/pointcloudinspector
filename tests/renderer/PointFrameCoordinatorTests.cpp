#include <pci/pointcloud/GpuPoint.h>
#include <pci/rendering/planning/PointFrameCoordinator.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

pci::PointBlockPtr flatBlock(const std::size_t pointCount)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->bounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    block->points.resize(pointCount);
    return block;
}

pci::PointFrameLayer flatLayer(const std::uint64_t layerId,
                               const pci::PointDatasetRuntimePtr &scene,
                               const pci::PointDatasetRuntimeSnapshot &snapshot)
{
    return {
        .layerId = pci::PointCloudLayerId{layerId},
        .sourceId = scene->sourceId(),
        .bindingGeneration = pci::BindingGeneration{layerId},
        .sourceBounds = snapshot.bounds,
        .residency = scene->residencyView(),
        .snapshot = &snapshot,
    };
}

pci::PointFrameLayer
hierarchyLayer(const std::uint64_t layerId,
               const pci::PointDatasetRuntimePtr &scene,
               const pci::PointDatasetRuntimeSnapshot &snapshot)
{
    return {
        .layerId = pci::PointCloudLayerId{layerId},
        .sourceId = scene->sourceId(),
        .bindingGeneration = pci::BindingGeneration{layerId},
        .sourceBounds = snapshot.bounds,
        .residency = scene->residencyView(),
        .snapshot = &snapshot,
    };
}

pci::FrameCamera visibleCamera()
{
    const pci::Vec3d eye{0.0, 0.0, 10.0};
    const pci::Vec3d forward{0.0, 0.0, -1.0};
    const pci::Vec3d up{0.0, 1.0, 0.0};
    const pci::Vec3d right{1.0, 0.0, 0.0};
    return {
        .eye = eye,
        .forward = forward,
        .up = up,
        .right = right,
        .culler = pci::FrustumCuller::fromOrthographic(
            eye, forward, up, right, 10.0, 1.0, 0.1, 100.0),
        .outputWidth = 100,
        .outputHeight = 100,
        .nearPlane = 0.1,
        .farPlane = 100.0,
        .orthographicScale = 20.0,
        .orthographic = true,
    };
}

class FailingHierarchySource final : public pci::PointCloudDataSource {
public:
    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        return {
            .id = id,
            .bounds = pci::pointCloudNodeBounds(
                {.minimum = {-1.0, -1.0, -1.0}, .maximum = {1.0, 1.0, 1.0}},
                id),
            .geometricError = id.level == 0 ? 1.0 : 0.0,
            .estimatedPointCount = 10,
            .leaf = id.level > 0,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(pci::PointCloudNodeId, std::stop_token) const override
    {
        ++loadCount;
        throw std::runtime_error("fixture hierarchy failed");
    }

    mutable std::atomic_uint64_t loadCount = 0;
};

pci::PointCloudNodePayloadPtr hierarchyRootPayload()
{
    auto payload = std::make_shared<pci::PointCloudNodePayload>();
    payload->nodeId = pci::rootPointCloudNode;
    payload->blocks.push_back(flatBlock(10));
    payload->sourcePointCount = 10;
    return payload;
}

TEST_CASE("point frame coordinator owns flat assembly and cache reuse",
          "[unit][renderer-planning][point-frame][golden]")
{
    auto scene =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    scene->addBlock(flatBlock(100));
    scene->markLoadingComplete();
    const pci::PointDatasetRuntimeSnapshot snapshot = scene->snapshot();
    const auto input = [&] {
        return pci::PointFrameInput{
            .sessionGeneration = pci::SessionGeneration{1},
            .documentGeneration = pci::DocumentGeneration{2},
            .documentRevision = 3,
            .layers = {flatLayer(7, scene, snapshot)},
            .camera = visibleCamera(),
            .cameraRevision = 3,
            .framePointBudget = 25,
            .gpuByteBudget = 100 * sizeof(pci::GpuPoint),
            .resident =
                [](const pci::PointFrameBlockKey &) {
                    return false;
                },
        };
    };

    pci::PointFrameCoordinator coordinator;
    const pci::PointFrameResult first = coordinator.plan(input());
    REQUIRE(first.plan);
    CHECK_FALSE(first.reused);
    REQUIRE(first.plan->blocks.size() == 1);
    CHECK(first.plan->blocks.front().layerId == pci::PointCloudLayerId{7});
    CHECK(first.plan->blocks.front().pointCount == 25);
    CHECK(first.plan->blocks.front().pointSpacing == 0.0);
    CHECK(first.plan->blocks.front().pointCoverageFactor == 1.0);
    CHECK(first.plan->uploads.size() == 1);
    CHECK(first.plan->protectedGpuBlocks.size() == 1);
    CHECK(first.plan->selectedPoints == 25);
    CHECK(first.plan->visibleLayerCount == 1);
    CHECK(first.plan->coveredLayerCount == 1);

    const pci::PointFrameResult second = coordinator.plan(input());
    CHECK(second.reused);
    CHECK(second.plan == first.plan);
    CHECK(second.execution.serial > first.execution.serial);
    CHECK(second.execution.sessionGeneration == pci::SessionGeneration{1});
    CHECK(second.execution.documentGeneration == pci::DocumentGeneration{2});

    pci::PointFrameInput changed = input();
    ++changed.cameraRevision;
    CHECK_FALSE(coordinator.plan(changed).reused);

    pci::PointFrameInput replacement = input();
    replacement.layers.front().sourceId =
        pci::PointCloudSourceId{scene->sourceId().value() + 100};
    replacement.layers.front().bindingGeneration = pci::BindingGeneration{8};
    CHECK_FALSE(coordinator.plan(replacement).reused);
}

TEST_CASE("point frame coordinator owns flat budget settlement",
          "[unit][renderer-planning][point-frame][budget]")
{
    auto scene =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    scene->addBlock(flatBlock(100));
    scene->markLoadingComplete();
    const pci::PointDatasetRuntimeSnapshot snapshot = scene->snapshot();
    const std::vector layers{flatLayer(1, scene, snapshot)};

    pci::PointFrameCoordinator coordinator;
    const pci::PointBudgetUpdate first = coordinator.pointBudgetUpdate(
        layers, 1024, 50 * sizeof(pci::GpuPoint), 10);
    CHECK(first.total == 50);
    CHECK(first.current == 50);
    const pci::PointBudgetUpdate unchanged = coordinator.pointBudgetUpdate(
        layers, 1024, 50 * sizeof(pci::GpuPoint), 20);
    CHECK(unchanged.total == 50);
    CHECK_FALSE(unchanged.current);
}

TEST_CASE("hierarchical documents recover the interactive bootstrap budget",
          "[unit][renderer-planning][point-frame][budget][regression]")
{
    pci::PointDatasetRuntimeSnapshot snapshot{
        .hierarchical = true,
        .loadingComplete = true,
        .sourcePointCount = 25'000'000,
    };
    const std::vector<pci::PointFrameLayer> layers{{
        .layerId = pci::PointCloudLayerId{1},
        .snapshot = &snapshot,
    }};

    pci::PointFrameCoordinator coordinator;
    const pci::PointBudgetUpdate published = coordinator.pointBudgetUpdate(
        layers, 512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 1);
    CHECK(published.total == 25'000'000);
    REQUIRE(published.current);
    CHECK(*published.current == 1'000'000);

    const pci::PointBudgetUpdate adapted = coordinator.pointBudgetUpdate(
        layers, 512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 1'500'000);
    REQUIRE(adapted.current);
    CHECK(*adapted.current == 1'500'000);
}

TEST_CASE("completed small hierarchies retain a stable total-point budget",
          "[unit][renderer-planning][point-frame][budget][regression]")
{
    pci::PointDatasetRuntimeSnapshot snapshot{
        .hierarchical = true,
        .loadingComplete = true,
        .sourcePointCount = 19'000'000,
    };
    const std::vector<pci::PointFrameLayer> layers{{
        .layerId = pci::PointCloudLayerId{1},
        .snapshot = &snapshot,
    }};

    pci::PointFrameCoordinator coordinator;
    const pci::PointBudgetUpdate result = coordinator.pointBudgetUpdate(
        layers, 512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 400'000);
    CHECK(result.total == 19'000'000);
    REQUIRE(result.current);
    CHECK(*result.current == result.total);

    const pci::PointBudgetUpdate gpuLimited = coordinator.pointBudgetUpdate(
        layers, 1024ULL * 1024 * 1024, 256ULL * 1024 * 1024, 400'000);
    REQUIRE(gpuLimited.current);
    CHECK(*gpuLimited.current == 1'000'000);
}

TEST_CASE("point frame planning records effects without executing runtime work",
          "[unit][renderer-planning][point-frame][architecture]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 80;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto source = std::make_shared<FailingHierarchySource>();
    auto scene = std::make_shared<pci::PointDatasetRuntime>(
        metadata, source, hierarchyRootPayload(), 1024 * 1024);
    scene->markLoadingComplete();
    const pci::PointDatasetRuntimeSnapshot snapshot = scene->snapshot();
    const pci::PointDatasetRuntimeMetrics before = scene->hierarchyMetrics();

    pci::PointFrameCoordinator coordinator;
    const pci::PointFrameResult frame = coordinator.plan({
        .layers = {hierarchyLayer(1, scene, snapshot)},
        .camera = visibleCamera(),
        .framePointBudget = 100,
        .gpuByteBudget = 100 * sizeof(pci::GpuPoint),
        .resident =
            [](const pci::PointFrameBlockKey &) {
                return true;
            },
    });

    REQUIRE(frame.plan);
    REQUIRE(frame.plan->nodeRequests.size() == 1);
    CHECK_FALSE(frame.plan->nodeRequests.front().nodes.empty());
    CHECK(frame.plan->nodeRequests.front().target.sourceId ==
          scene->sourceId());
    CHECK(frame.plan->nodeRequests.front().target.bindingGeneration ==
          pci::BindingGeneration{1});
    REQUIRE(frame.plan->trimRequests.size() == 1);
    CHECK_FALSE(frame.plan->trimRequests.front().protectedNodes.empty());
    const pci::PointDatasetRuntimeMetrics after = scene->hierarchyMetrics();
    CHECK(after.decodeRequestsQueued == before.decodeRequestsQueued);
    CHECK(after.decodeRequestsStarted == before.decodeRequestsStarted);
    CHECK(after.cache.hits == before.cache.hits);
    CHECK(after.cache.misses == before.cache.misses);
    CHECK(after.cache.evictions == before.cache.evictions);
    CHECK(source->loadCount.load() == 0);

    REQUIRE_FALSE(frame.plan->decodedLookupEffects.empty());
    const auto residentLookups = static_cast<std::uint64_t>(
        std::ranges::count(frame.plan->decodedLookupEffects,
                           true,
                           &pci::PointFrameDecodedLookupEffect::resident));
    for (const pci::PointFrameDecodedLookupEffect &effect :
         frame.plan->decodedLookupEffects) {
        scene->applyDecodedLookupEffect(effect.nodeId, effect.resident);
    }
    const pci::PointDatasetRuntimeMetrics applied = scene->hierarchyMetrics();
    CHECK(applied.cache.hits == before.cache.hits + residentLookups);
    CHECK(applied.cache.misses == before.cache.misses +
                                      frame.plan->decodedLookupEffects.size() -
                                      residentLookups);
}

TEST_CASE("point frame coordinator isolates a failed hierarchy layer",
          "[unit][renderer-planning][point-frame][failure]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 80;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto failedScene = std::make_shared<pci::PointDatasetRuntime>(
        metadata,
        std::make_shared<FailingHierarchySource>(),
        hierarchyRootPayload(),
        1024 * 1024);
    failedScene->markLoadingComplete();
    const pci::PointDatasetRuntimeSnapshot coarseSnapshot =
        failedScene->snapshot();
    pci::PointFrameCoordinator coarseCoordinator;
    const pci::PointFrameResult coarseFrame = coarseCoordinator.plan({
        .layers = {hierarchyLayer(1, failedScene, coarseSnapshot)},
        .camera = visibleCamera(),
        .framePointBudget = 100,
        .gpuByteBudget = 100 * sizeof(pci::GpuPoint),
        .resident =
            [](const pci::PointFrameBlockKey &) {
                return true;
            },
    });
    REQUIRE(coarseFrame.plan);
    CHECK(coarseFrame.plan->coveredLayerCount == 1);
    CHECK(coarseFrame.plan->rootOnlyLayerCount == 1);

    failedScene->requestNodes(
        std::array{pci::childNodeId(pci::rootPointCloudNode, 0)});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (failedScene->hierarchyError().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        failedScene->drainDecodeCompletions();
        std::this_thread::yield();
    }
    REQUIRE(failedScene->hierarchyError() == "fixture hierarchy failed");

    auto healthyScene =
        std::make_shared<pci::PointDatasetRuntime>(pci::PointCloudMetadata{});
    healthyScene->addBlock(flatBlock(10));
    healthyScene->markLoadingComplete();
    const pci::PointDatasetRuntimeSnapshot failedSnapshot =
        failedScene->snapshot();
    const pci::PointDatasetRuntimeSnapshot healthySnapshot =
        healthyScene->snapshot();
    const std::vector<pci::PointFrameLayer> layers{
        hierarchyLayer(1, failedScene, failedSnapshot),
        flatLayer(2, healthyScene, healthySnapshot),
    };

    pci::PointFrameCoordinator coordinator;
    const pci::PointFrameResult frame = coordinator.plan({
        .layers = layers,
        .camera = visibleCamera(),
        .framePointBudget = 100,
        .gpuByteBudget = 100 * sizeof(pci::GpuPoint),
        .resident =
            [](const pci::PointFrameBlockKey &) {
                return true;
            },
    });
    REQUIRE(frame.plan);
    REQUIRE(frame.plan->layerErrors.size() == 1);
    CHECK(frame.plan->layerErrors.front().layerId == pci::PointCloudLayerId{1});
    CHECK(frame.plan->layerErrors.front().message ==
          "fixture hierarchy failed");
    CHECK(std::ranges::any_of(frame.plan->blocks, [](const auto &block) {
        return block.layerId == pci::PointCloudLayerId{2};
    }));
}

} // namespace
