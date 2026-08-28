#include "pointcloud/GpuPoint.h"
#include "renderer/planning/PointFrameCoordinator.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

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

} // namespace
