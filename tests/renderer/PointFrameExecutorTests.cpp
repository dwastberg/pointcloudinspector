#include <pci/rendering/PointFrameExecutor.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stop_token>

namespace {

class InertHierarchySource final : public pci::PointCloudDataSource {
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
            .bounds = {.minimum = {-1.0, -1.0, -1.0},
                       .maximum = {1.0, 1.0, 1.0}},
            .geometricError = id.level == 0 ? 1.0 : 0.0,
            .estimatedPointCount = 10,
            .leaf = id.level > 0,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(pci::PointCloudNodeId, std::stop_token) const override
    {
        ++loadCount;
        return {};
    }

    mutable std::uint64_t loadCount = 0;
};

pci::PointCloudNodePayloadPtr rootPayload()
{
    auto block = std::make_shared<pci::PointBlock>();
    block->bounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    block->points.resize(10);
    auto payload = std::make_shared<pci::PointCloudNodePayload>();
    payload->nodeId = pci::rootPointCloudNode;
    payload->blocks.push_back(std::move(block));
    payload->sourcePointCount = 10;
    return payload;
}

pci::PointDatasetRuntimePtr hierarchyScene()
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 10;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    return std::make_shared<pci::PointDatasetRuntime>(
        metadata,
        std::make_shared<InertHierarchySource>(),
        rootPayload(),
        1024 * 1024);
}

pci::PointFrameResult frameFor(const pci::PointDatasetRuntime &scene,
                               const pci::BindingGeneration binding,
                               const std::uint64_t serial)
{
    const pci::PointFrameRuntimeTarget target{
        .layerId = pci::PointCloudLayerId{7},
        .sourceId = scene.sourceId(),
        .bindingGeneration = binding,
        .contentRevision = scene.residencyContentRevision(),
    };
    auto plan = std::make_shared<pci::PointFramePlan>();
    plan->nodeRequests.push_back({.target = target, .nodes = {}});
    plan->decodedLookupEffects.push_back({
        .target = target,
        .nodeId = pci::rootPointCloudNode,
        .resident = true,
    });
    plan->trimRequests.push_back(
        {.target = target, .protectedNodes = {pci::rootPointCloudNode}});
    return {
        .plan = std::move(plan),
        .execution = {.sessionGeneration = pci::SessionGeneration{2},
                      .documentGeneration = pci::DocumentGeneration{3},
                      .documentRevision = 4,
                      .serial = serial},
    };
}

pci::SceneRuntimeSnapshotPtr
runtimeFor(const pci::PointDatasetRuntimePtr &scene,
           const pci::BindingGeneration binding)
{
    pci::SceneRuntime runtime;
    REQUIRE(runtime.attachPoint({.descriptor = scene->descriptor(),
                                 .runtime = scene,
                                 .generation = binding}));
    return runtime.snapshot();
}

pci::PointFrameExecutionContext context(pci::SceneRuntimeSnapshotPtr runtime)
{
    return {
        .sessionGeneration = pci::SessionGeneration{2},
        .documentGeneration = pci::DocumentGeneration{3},
        .documentRevision = 4,
        .runtime = std::move(runtime),
    };
}

TEST_CASE("point frame effects execute once after complete identity validation",
          "[unit][renderer-execution][point-frame][architecture]")
{
    const pci::BindingGeneration binding{5};
    const pci::PointDatasetRuntimePtr scene = hierarchyScene();
    const pci::PointFrameResult frame = frameFor(*scene, binding, 1);
    const pci::PointFrameExecutionContext current =
        context(runtimeFor(scene, binding));
    const pci::PointDatasetRuntimeMetrics before = scene->hierarchyMetrics();

    pci::PointFrameExecutor executor;
    CHECK(executor.execute(frame, current) ==
          pci::PointFrameExecutionOutcome::Applied);
    const pci::PointDatasetRuntimeMetrics applied = scene->hierarchyMetrics();
    CHECK(applied.cache.hits == before.cache.hits + 1);
    CHECK(executor.execute(frame, current) ==
          pci::PointFrameExecutionOutcome::DuplicateOrOutOfOrder);
    CHECK(scene->hierarchyMetrics().cache.hits == applied.cache.hits);
}

TEST_CASE("point frame execution rejects document binding and content changes",
          "[unit][renderer-execution][point-frame][stale]")
{
    const pci::BindingGeneration binding{5};
    const pci::PointDatasetRuntimePtr original = hierarchyScene();
    const pci::PointFrameResult originalFrame = frameFor(*original, binding, 1);

    SECTION("document identity")
    {
        pci::PointFrameExecutionContext changed =
            context(runtimeFor(original, binding));
        ++changed.documentRevision;
        const auto before = original->hierarchyMetrics().cache;
        pci::PointFrameExecutor executor;
        CHECK(executor.execute(originalFrame, changed) ==
              pci::PointFrameExecutionOutcome::Stale);
        CHECK(original->hierarchyMetrics().cache.hits == before.hits);
    }

    SECTION("source binding")
    {
        const pci::PointDatasetRuntimePtr replacement = hierarchyScene();
        const auto replacementBefore = replacement->hierarchyMetrics().cache;
        pci::PointFrameExecutor executor;
        CHECK(executor.execute(originalFrame,
                               context(runtimeFor(replacement, binding))) ==
              pci::PointFrameExecutionOutcome::Stale);
        CHECK(replacement->hierarchyMetrics().cache.hits ==
              replacementBefore.hits);
    }

    SECTION("content revision")
    {
        const auto minimumBytes = original->minimumRootPayloadBytes();
        REQUIRE(original->limitRootPayloadBytes(minimumBytes) == minimumBytes);
        const auto changedBefore = original->hierarchyMetrics().cache;
        pci::PointFrameExecutor executor;
        CHECK(executor.execute(originalFrame,
                               context(runtimeFor(original, binding))) ==
              pci::PointFrameExecutionOutcome::Stale);
        CHECK(original->hierarchyMetrics().cache.hits == changedBefore.hits);
    }
}

} // namespace
