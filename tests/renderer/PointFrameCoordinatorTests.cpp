#include "pointcloud/GpuPoint.h"
#include "renderer/planning/PointFrameCoordinator.h"

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
                               const pci::PointCloudScenePtr &scene,
                               const pci::PointCloudSceneSnapshot &snapshot)
{
    return {
        .layer = {.id = pci::PointCloudLayerId{layerId}, .scene = scene},
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
                {.minimum = {-1.0, -1.0, -1.0},
                 .maximum = {1.0, 1.0, 1.0}},
                id),
            .geometricError = id.level == 0 ? 1.0 : 0.0,
            .estimatedPointCount = 10,
            .leaf = id.level > 0,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(pci::PointCloudNodeId, std::stop_token) const override
    {
        throw std::runtime_error("fixture hierarchy failed");
    }
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
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{});
    scene->addBlock(flatBlock(100));
    scene->markLoadingComplete();
    const pci::PointCloudSceneSnapshot snapshot = scene->snapshot();
    const auto input = [&] {
        return pci::PointFrameInput{
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

    pci::PointFrameInput changed = input();
    ++changed.cameraRevision;
    CHECK_FALSE(coordinator.plan(changed).reused);
}

TEST_CASE("point frame coordinator owns flat budget settlement",
          "[unit][renderer-planning][point-frame][budget]")
{
    auto scene =
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{});
    scene->addBlock(flatBlock(100));
    scene->markLoadingComplete();
    const pci::PointCloudSceneSnapshot snapshot = scene->snapshot();
    const std::vector layers{flatLayer(1, scene, snapshot)};

    pci::PointFrameCoordinator coordinator;
    const pci::PointBudgetUpdate first =
        coordinator.pointBudgetUpdate(layers, 50 * sizeof(pci::GpuPoint), 10);
    CHECK(first.total == 50);
    CHECK(first.current == 50);
    const pci::PointBudgetUpdate unchanged =
        coordinator.pointBudgetUpdate(layers, 50 * sizeof(pci::GpuPoint), 20);
    CHECK(unchanged.total == 50);
    CHECK_FALSE(unchanged.current);
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
    auto failedScene = std::make_shared<pci::PointCloudScene>(
        metadata,
        std::make_shared<FailingHierarchySource>(),
        hierarchyRootPayload(),
        1024 * 1024);
    failedScene->markLoadingComplete();
    failedScene->requestNodes(
        std::array{pci::childNodeId(pci::rootPointCloudNode, 0)});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (failedScene->hierarchyError().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(failedScene->hierarchyError() == "fixture hierarchy failed");

    auto healthyScene =
        std::make_shared<pci::PointCloudScene>(pci::PointCloudMetadata{});
    healthyScene->addBlock(flatBlock(10));
    healthyScene->markLoadingComplete();
    const pci::PointCloudSceneSnapshot failedSnapshot = failedScene->snapshot();
    const pci::PointCloudSceneSnapshot healthySnapshot =
        healthyScene->snapshot();
    const std::vector<pci::PointFrameLayer> layers{
        {.layer = {.id = pci::PointCloudLayerId{1}, .scene = failedScene},
         .snapshot = &failedSnapshot},
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
    CHECK(frame.plan->layerErrors.front().layerId ==
          pci::PointCloudLayerId{1});
    CHECK(frame.plan->layerErrors.front().message ==
          "fixture hierarchy failed");
    CHECK(std::ranges::any_of(frame.plan->blocks, [](const auto &block) {
        return block.layerId == pci::PointCloudLayerId{2};
    }));
}

} // namespace
