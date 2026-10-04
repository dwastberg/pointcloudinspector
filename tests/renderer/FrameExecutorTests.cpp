#include <pci/rendering/FrameExecutor.h>
#include <pci/rendering/FramePickGuard.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

namespace pci {
struct FrameExecutorTestAccess {
    static void protect(FrameExecutor &executor,
                        const FrameContext &context,
                        std::span<const PointFrameTrimRequest> requests)
    {
        executor.protectPoints(context, requests);
    }
    static void drain(FrameExecutor &executor, const FrameContext &context)
    {
        executor.drain(context);
    }
    static std::size_t retainedBytes(const FrameExecutor &executor)
    {
        return executor.protectionScratch_.capacity() * sizeof(DecodedPageKey) +
               executor.requestScratch_.capacity() * sizeof(RasterRequestBatch);
    }
    static void finish(FrameExecutor &executor)
    {
        executor.finishScratch();
    }
    static std::size_t growths(const FrameExecutor &executor)
    {
        return executor.scratchGrowths_;
    }
    static std::size_t identityBuilds(const FrameExecutor &executor)
    {
        return executor.rasterIdentityBuilds_;
    }
};
} // namespace pci

namespace {
class PreparedSource final : public pci::PointCloudDataSource {
public:
    pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }
    pci::PointCloudNode node(pci::PointCloudNodeId id) const override
    {
        return {.id = id, .estimatedPointCount = 1, .leaf = true};
    }
    pci::PointCloudNodePayloadPtr loadNode(pci::PointCloudNodeId id,
                                           std::stop_token) const override
    {
        auto block = std::make_shared<pci::PointBlock>();
        block->points.resize(id.level == 0 ? 4 : 1);
        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->blocks.push_back(std::move(block));
        return result;
    }
};

struct Fixture {
    std::shared_ptr<PreparedSource> source = std::make_shared<PreparedSource>();
    pci::SceneRuntime runtime{1024 * 1024};
    pci::PointDatasetRuntimePtr point =
        std::make_shared<pci::PointDatasetRuntime>(
            pci::PointCloudMetadata{},
            source,
            source->loadNode(pci::rootPointCloudNode, {}));
    std::shared_ptr<pci::SceneDocumentSnapshot> document =
        std::make_shared<pci::SceneDocumentSnapshot>();
    pci::FrameContext context;
    Fixture()
    {
        REQUIRE(runtime.attachPoint({.descriptor = point->descriptor(),
                                     .runtime = point,
                                     .generation = pci::BindingGeneration{1}}));
        document->generation = pci::DocumentGeneration{1};
        document->revision = 1;
        document->layers.push_back(
            {.id = pci::SceneLayerId{1},
             .payload =
                 pci::PointCloudLayerSnapshotState{.descriptor =
                                                       point->descriptor()},
             .bindingGeneration = pci::BindingGeneration{1}});
        document->layerIndices.emplace(pci::SceneLayerId{1}, 0);
        document->pointLayerIndices.push_back(0);
        context = {pci::SessionGeneration{1}, document, runtime.snapshot()};
    }
    pci::FramePlanningInput input() const
    {
        pci::FramePlanningInput result;
        result.points.sessionGeneration = context.session;
        result.points.documentGeneration = document->generation;
        result.points.documentRevision = document->revision;
        return result;
    }
};
} // namespace

TEST_CASE("frame execution admits prepared work before capture and protects "
          "before submission",
          "[frame][completion][unit]")
{
    Fixture fixture;
    const auto child = pci::childNodeId(pci::rootPointCloudNode, 0);
    fixture.point->requestNodes(std::array{child});
    fixture.runtime.hierarchyScheduler()->waitForIdle();
    REQUIRE_FALSE(fixture.point->peekNodePayload(child));
    pci::FrameExecutor executor;
    std::vector<std::string> trace;
    executor.run({
        .context =
            [&] {
                return fixture.context;
            },
        .capture =
            [&] {
                REQUIRE(fixture.point->peekNodePayload(child));
                trace.emplace_back("capture");
                return fixture.input();
            },
        .protect =
            [&](const auto &, const auto &) {
                trace.emplace_back("protect");
            },
        .submit =
            [&](const auto &, const auto &) {
                trace.emplace_back("submit");
            },
        .complete =
            [&](bool) {
                trace.emplace_back("complete");
            },
    });
    CHECK(trace ==
          std::vector<std::string>{"capture", "protect", "submit", "complete"});
}

TEST_CASE("frame replacement between capture and execution rejects all effects",
          "[frame][stale][unit]")
{
    Fixture fixture;
    pci::FrameExecutor executor;
    bool continued = false;
    executor.run({
        .context =
            [&] {
                return fixture.context;
            },
        .capture =
            [&] {
                auto result = fixture.input();
                fixture.context.session = pci::SessionGeneration{2};
                return result;
            },
        .protect =
            [&](const auto &, const auto &) {
                FAIL("stale protection executed");
            },
        .submit =
            [&](const auto &, const auto &) {
                FAIL("stale upload executed");
            },
        .complete =
            [&](bool continuation) {
                continued = continuation;
            },
    });
    CHECK(continued);
}

TEST_CASE("prepared point completions drain in bounded batches and discard "
          "without owner pumping",
          "[frame][completion][shutdown][unit]")
{
    Fixture fixture;
    std::vector<pci::PointCloudNodeId> nodes;
    nodes.reserve(65);
    for (std::uint32_t i = 0; i < 65; ++i) {
        nodes.push_back({.level = 3, .x = i % 8, .y = i / 8, .z = 0});
    }
    fixture.point->requestNodes(nodes);
    fixture.runtime.hierarchyScheduler()->waitForIdle();
    CHECK(fixture.point->hierarchyMetrics().decodeRequestsQueued == 64);
    CHECK(fixture.runtime.decodedPageCache()->size() == 1);
    CHECK(fixture.point->drainDecodeCompletions(3) == 3);
    CHECK(fixture.runtime.decodedPageCache()->size() == 4);
    fixture.point->quiesceForAttachment();
    CHECK_FALSE(fixture.point->decodeInFlight());
    CHECK(fixture.runtime.memoryBudget()->reservedBytes() == 0);
}

TEST_CASE("pick guards reject content replacement even without a document "
          "revision change",
          "[frame][pick][stale][unit]")
{
    Fixture fixture;
    const auto guard = pci::FramePickGuard::capture(fixture.context);
    REQUIRE(guard.valid(fixture.context));
    SECTION("session")
    {
        fixture.context.session = pci::SessionGeneration{2};
    }
    SECTION("document")
    {
        fixture.document->generation = pci::DocumentGeneration{2};
    }
    SECTION("binding")
    {
        fixture.document->layers.front().bindingGeneration =
            pci::BindingGeneration{2};
    }
    SECTION("color")
    {
        ++std::get<pci::PointCloudLayerSnapshotState>(
              fixture.document->layers.front().payload)
              .colorGeneration;
    }
    SECTION("root")
    {
        static_cast<void>(fixture.point->limitRootPayloadBytes(
            fixture.point->minimumRootPayloadBytes()));
    }
    CHECK_FALSE(guard.valid(fixture.context));
}

TEST_CASE("stable frame metadata reuses scratch and raster identity indexing",
          "[frame][scratch][complexity][unit]")
{
    Fixture fixture;
    pci::FrameExecutor executor;
    const std::array requests{pci::PointFrameTrimRequest{
        .target = {.layerId = pci::PointCloudLayerId{1},
                   .sourceId = fixture.point->descriptor().sourceId,
                   .bindingGeneration = pci::BindingGeneration{1},
                   .contentRevision =
                       fixture.point->residencyContentRevision()},
        .protectedNodes = {pci::rootPointCloudNode}}};
    pci::FrameExecutorTestAccess::drain(executor, fixture.context);
    pci::FrameExecutorTestAccess::protect(executor, fixture.context, requests);
    const auto growths = pci::FrameExecutorTestAccess::growths(executor);
    for (int i = 0; i < 8; ++i) {
        pci::FrameExecutorTestAccess::protect(
            executor, fixture.context, requests);
        pci::FrameExecutorTestAccess::drain(executor, fixture.context);
    }
    CHECK(pci::FrameExecutorTestAccess::growths(executor) == growths);
    CHECK(pci::FrameExecutorTestAccess::identityBuilds(executor) == 1);
    ++fixture.document->revision;
    pci::FrameExecutorTestAccess::drain(executor, fixture.context);
    CHECK(pci::FrameExecutorTestAccess::identityBuilds(executor) == 2);
}

TEST_CASE("frame scratch releases excessive capacity on replacement and budget "
          "shrink",
          "[frame][scratch][budget][unit]")
{
    Fixture fixture;
    pci::FrameExecutor executor;
    using Access = pci::FrameExecutorTestAccess;
    Access::drain(executor, fixture.context);
    std::array requests{pci::PointFrameTrimRequest{
        .target = {.layerId = pci::PointCloudLayerId{1},
                   .sourceId = fixture.point->descriptor().sourceId,
                   .bindingGeneration = pci::BindingGeneration{1},
                   .contentRevision =
                       fixture.point->residencyContentRevision()},
        .protectedNodes =
            std::vector<pci::PointCloudNodeId>(4096, pci::rootPointCloudNode)}};
    Access::protect(executor, fixture.context, requests);
    REQUIRE(Access::retainedBytes(executor) > 16384);
    Access::finish(executor);
    CHECK(Access::retainedBytes(executor) == 0);
    requests.front().protectedNodes.resize(4);
    Access::protect(executor, fixture.context, requests);
    Access::finish(executor);
    CHECK(Access::retainedBytes(executor) > 0);
    REQUIRE(fixture.runtime.memoryBudget()->setByteBudget(1024));
    Access::drain(executor, fixture.context);
    CHECK(Access::retainedBytes(executor) == 0);
    Access::protect(executor, fixture.context, requests);
    executor.clear();
    CHECK(Access::retainedBytes(executor) == 0);
    CHECK(fixture.point->peekNodePayload(pci::rootPointCloudNode));
}
