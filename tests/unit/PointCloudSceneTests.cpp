#include "scene/PointCloudScene.h"
#include "scene/SceneDocument.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

pci::PointBlockPtr blockWith(const std::size_t pointCount,
                             const pci::Vec3d origin,
                             const std::uint16_t intensityMaximum = 0)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = origin;
    block->scale = 1.0 / 65535.0;
    block->bounds = {
        .minimum = {origin.x, origin.y, origin.z},
        .maximum = {origin.x + 1.0, origin.y + 1.0, origin.z + 1.0},
    };
    block->intensityMinimum = intensityMaximum;
    block->intensityMaximum = intensityMaximum;
    block->points.resize(pointCount);
    block->attributes.resize(pointCount);
    return block;
}

class FakeHierarchySource final : public pci::PointCloudDataSource {
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
            .bounds = pci::pointCloudNodeBounds(bounds_, id),
            .geometricError = id.level == 0 ? 1.0 : 0.0,
            .estimatedPointCount = id.level == 0 ? 1U : 4U,
            .leaf = id.level != 0,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId id,
             const std::stop_token stopToken) const override
    {
        ++loadCount;
        if (stopToken.stop_requested()) {
            throw pci::PointCloudDataSourceCancelled();
        }
        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->sourcePointCount = 4;
        const auto nodeBounds = pci::pointCloudNodeBounds(bounds_, id);
        result->blocks.push_back(blockWith(4,
                                           {nodeBounds.minimum[0],
                                            nodeBounds.minimum[1],
                                            nodeBounds.minimum[2]},
                                           900));
        return result;
    }

    mutable std::atomic_uint32_t loadCount = 0;

private:
    const pci::Bounds3d bounds_{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
};

class CancellableHierarchySource final : public pci::PointCloudDataSource {
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
        if (id == pci::childNodeId(pci::rootPointCloudNode, 0)) {
            obsoleteStarted = true;
            while (!stopToken.stop_requested()) {
                std::this_thread::yield();
            }
            obsoleteCancelled = true;
            throw pci::PointCloudDataSourceCancelled();
        }
        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->sourcePointCount = 4;
        result->blocks.push_back(blockWith(4, {1.0, 0.0, 0.0}));
        replacementLoaded = true;
        return result;
    }

    mutable std::atomic_bool obsoleteStarted = false;
    mutable std::atomic_bool obsoleteCancelled = false;
    mutable std::atomic_bool replacementLoaded = false;

private:
    const pci::Bounds3d bounds_{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
};

class GatedHierarchySource final : public pci::PointCloudDataSource {
public:
    explicit GatedHierarchySource(const bool waitsForRelease)
        : waitsForRelease_(waitsForRelease)
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
        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->sourcePointCount = 4;
        result->blocks.push_back(blockWith(4, {0.0, 0.0, 0.0}));
        return result;
    }

    mutable std::atomic_bool started = false;
    mutable std::atomic_bool released = false;

private:
    bool waitsForRelease_ = false;
    const pci::Bounds3d bounds_{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
};

class StressHierarchySource final : public pci::PointCloudDataSource {
public:
    explicit StressHierarchySource(
        const std::uint8_t maximumLevel = 2,
        const std::size_t pointsPerNode = 64,
        const std::chrono::milliseconds decodeDelay = {})
        : maximumLevel_(maximumLevel)
        , pointsPerNode_(pointsPerNode)
        , decodeDelay_(decodeDelay)
    {
    }

    [[nodiscard]] pci::PointCloudNode rootNode() const override
    {
        return node(pci::rootPointCloudNode);
    }

    [[nodiscard]] pci::PointCloudNode
    node(const pci::PointCloudNodeId id) const override
    {
        const std::uint64_t cells = std::uint64_t{1} << id.level;
        if (id.level > maximumLevel_ || id.x >= cells || id.y >= cells ||
            id.z >= cells) {
            throw std::out_of_range("stress hierarchy node is invalid");
        }
        return {
            .id = id,
            .bounds = pci::pointCloudNodeBounds(bounds_, id),
            .geometricError =
                id.level < maximumLevel_
                    ? 1.0 / static_cast<double>(std::uint64_t{1} << id.level)
                    : 0.0,
            .estimatedPointCount = pointsPerNode_,
            .leaf = id.level == maximumLevel_,
        };
    }

    [[nodiscard]] pci::PointCloudNodePayloadPtr
    loadNode(const pci::PointCloudNodeId id,
             const std::stop_token stopToken) const override
    {
        const auto started = std::chrono::steady_clock::now();
        ++requests_;
        {
            const std::scoped_lock lock(mutex_);
            ++loadCounts_[id];
        }
        const auto recordDuration = [this, started] {
            totalQueryNanoseconds_.fetch_add(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count()),
                std::memory_order_relaxed);
        };
        const auto deadline = started + decodeDelay_;
        while (std::chrono::steady_clock::now() < deadline) {
            if (stopToken.stop_requested()) {
                ++cancelled_;
                recordDuration();
                throw pci::PointCloudDataSourceCancelled();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stopToken.stop_requested()) {
            ++cancelled_;
            recordDuration();
            throw pci::PointCloudDataSourceCancelled();
        }

        const pci::Bounds3d nodeBounds = node(id).bounds;
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {
            nodeBounds.minimum[0],
            nodeBounds.minimum[1],
            nodeBounds.minimum[2],
        };
        block->scale = nodeBounds.maximumExtent() / 65535.0;
        block->bounds = nodeBounds;
        block->points.resize(pointsPerNode_);
        block->attributes.resize(pointsPerNode_);
        auto result = std::make_shared<pci::PointCloudNodePayload>();
        result->nodeId = id;
        result->sourcePointCount = pointsPerNode_;
        result->blocks.push_back(std::move(block));
        const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*result);
        decodedBytes_.fetch_add(bytes, std::memory_order_relaxed);
        ++completed_;
        recordDuration();
        return result;
    }

    [[nodiscard]] pci::PointCloudDataSourceMetrics metrics() const override
    {
        return {
            .requests = requests_.load(std::memory_order_relaxed),
            .completed = completed_.load(std::memory_order_relaxed),
            .cancelled = cancelled_.load(std::memory_order_relaxed),
            .estimatedDecodedBytesRequested =
                requests_.load(std::memory_order_relaxed) * pointsPerNode_ *
                (sizeof(pci::GpuPoint) + sizeof(pci::PointAttributes)),
            .decodedBytesProduced =
                decodedBytes_.load(std::memory_order_relaxed),
            .decodedPointsProduced =
                completed_.load(std::memory_order_relaxed) * pointsPerNode_,
            .totalQueryNanoseconds =
                totalQueryNanoseconds_.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] std::uint64_t loadCount(const pci::PointCloudNodeId id) const
    {
        const std::scoped_lock lock(mutex_);
        const auto found = loadCounts_.find(id);
        return found == loadCounts_.end() ? 0 : found->second;
    }

private:
    std::uint8_t maximumLevel_ = 2;
    std::size_t pointsPerNode_ = 64;
    std::chrono::milliseconds decodeDelay_{};
    const pci::Bounds3d bounds_{
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    mutable std::atomic_uint64_t requests_ = 0;
    mutable std::atomic_uint64_t completed_ = 0;
    mutable std::atomic_uint64_t cancelled_ = 0;
    mutable std::atomic_uint64_t decodedBytes_ = 0;
    mutable std::atomic_uint64_t totalQueryNanoseconds_ = 0;
    mutable std::mutex mutex_;
    mutable std::unordered_map<pci::PointCloudNodeId,
                               std::uint64_t,
                               pci::PointCloudNodeIdHash>
        loadCounts_;
};

pci::PointCloudNodePayloadPtr rootPayload()
{
    auto result = std::make_shared<pci::PointCloudNodePayload>();
    result->nodeId = pci::rootPointCloudNode;
    result->sourcePointCount = 8;
    result->blocks.push_back(blockWith(1, {0.0, 0.0, 0.0}, 100));
    return result;
}

TEST_CASE("scene aggregates blocks, bounds, and statistics", "[unit][scene]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 5;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {4.0, 4.0, 4.0},
    };
    pci::PointCloudScene scene(metadata);

    CHECK(scene.totalPointCount() == 0);
    CHECK(scene.revision() == 0);
    CHECK(scene.bounds().minimum == metadata.sourceBounds.minimum);

    scene.addBlock(blockWith(3, {0.0, 0.0, 0.0}, 100));
    scene.addBlock(blockWith(2, {2.0, 2.0, 2.0}, 700));

    CHECK(scene.metadata().sourcePointCount == 5);
    CHECK(scene.totalPointCount() == 5);
    CHECK(scene.revision() == 2);
    CHECK(scene.blocks().size() == 2);
    CHECK(scene.intensityMinimum() == 100);
    CHECK(scene.intensityMaximum() == 700);
    CHECK(scene.bounds().minimum == std::array{0.0, 0.0, 0.0});
    CHECK(scene.bounds().maximum == std::array{3.0, 3.0, 3.0});
}

TEST_CASE("scene records classifications observed in published blocks",
          "[unit][scene][classification]")
{
    pci::PointCloudMetadata metadata;
    metadata.hasClassification = true;
    pci::PointCloudScene scene(metadata);

    auto block = std::make_shared<pci::PointBlock>();
    block->points = {
        {.attributes = 2},
        {.attributes = 6},
        {.attributes = 6},
        {.attributes = 17},
    };
    block->attributes.resize(block->points.size());
    scene.addBlock(std::move(block));

    const pci::PointClassificationFilter classifications =
        scene.presentClassifications();
    CHECK(classifications.visibleCount() == 3);
    CHECK(classifications.isVisible(2));
    CHECK(classifications.isVisible(6));
    CHECK(classifications.isVisible(17));
    CHECK_FALSE(classifications.isVisible(1));
}

TEST_CASE("flat scene publishes exact intensity range only at completion",
          "[unit][scene][color]")
{
    pci::PointCloudMetadata metadata;
    metadata.hasIntensity = true;
    metadata.sourcePointCount = 4;
    pci::PointCloudScene scene(metadata);
    scene.addBlock(blockWith(2, {0.0, 0.0, 0.0}, 200));
    scene.addBlock(blockWith(2, {1.0, 0.0, 0.0}, 700));

    CHECK_FALSE(scene.scalarRanges().intensity);
    scene.markLoadingComplete();
    REQUIRE(scene.scalarRanges().intensity);
    CHECK(*scene.scalarRanges().intensity ==
          pci::PointScalarRange{200.0, 700.0});
    CHECK(scene.snapshot().scalarRanges == scene.scalarRanges());
}

TEST_CASE("sampled flat scene does not claim a source-wide intensity range",
          "[unit][scene][color]")
{
    pci::PointCloudMetadata metadata;
    metadata.hasIntensity = true;
    metadata.sourcePointCount = 10;
    pci::PointCloudScene scene(metadata);
    scene.addBlock(blockWith(4, {0.0, 0.0, 0.0}, 700));
    scene.markLoadingComplete();

    CHECK_FALSE(scene.scalarRanges().intensity);
}

TEST_CASE("scene accepts blocks from a worker thread", "[unit][scene]")
{
    pci::PointCloudScene scene({});
    std::thread worker([&scene] {
        for (int i = 0; i < 100; ++i) {
            scene.addBlock(blockWith(10, {0.0, 0.0, 0.0}));
        }
    });
    // Concurrent reads must be safe while the worker adds blocks.
    while (scene.totalPointCount() < 1000) {
        static_cast<void>(scene.blocks().size());
    }
    worker.join();
    CHECK(scene.totalPointCount() == 1000);
    CHECK(scene.revision() == 100);
}

TEST_CASE("scene snapshots stay consistent during worker publication",
          "[unit][scene][snapshot]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 1'000;
    pci::PointCloudScene scene(metadata);
    std::atomic_bool finished = false;
    std::thread worker([&scene, &finished] {
        for (std::uint16_t index = 1; index <= 100; ++index) {
            scene.addBlock(
                blockWith(10, {static_cast<double>(index), 0.0, 0.0}, index));
        }
        finished = true;
    });

    do {
        const pci::PointCloudSceneSnapshot snapshot = scene.snapshot();
        CHECK_FALSE(snapshot.hierarchical);
        CHECK(snapshot.sourcePointCount == 1'000);
        CHECK(snapshot.revision == snapshot.flatBlocks.size());
        CHECK(snapshot.retainedFlatPointCount ==
              snapshot.flatBlocks.size() * 10);
        CHECK(snapshot.decodedResidentPointCount ==
              snapshot.retainedFlatPointCount);
        if (!snapshot.flatBlocks.empty()) {
            CHECK(snapshot.intensityMaximum == snapshot.flatBlocks.size());
            CHECK(snapshot.bounds.valid());
        }
    } while (!finished.load());

    worker.join();
    const pci::PointCloudSceneSnapshot complete = scene.snapshot();
    CHECK(complete.revision == 100);
    CHECK(complete.flatBlocks.size() == 100);
    CHECK(complete.retainedFlatPointCount == 1'000);
    CHECK_FALSE(complete.loadingComplete);
    scene.markLoadingComplete();
    const pci::PointCloudSceneSnapshot marked = scene.snapshot();
    CHECK(marked.loadingComplete);
    CHECK(marked.revision == 101);
}

TEST_CASE("scene invalidation follows committed worker publications",
          "[unit][scene][invalidation]")
{
    pci::PointCloudScene scene({});
    std::atomic_uint32_t notifications = 0;
    auto subscription = scene.subscribeInvalidation([&notifications] {
        ++notifications;
    });
    REQUIRE(subscription);

    std::thread worker([&scene] {
        scene.addBlock(blockWith(4, {0.0, 0.0, 0.0}));
    });
    worker.join();
    CHECK(scene.revision() == 1);
    CHECK(scene.totalPointCount() == 4);
    CHECK(notifications == 1);

    subscription.reset();
    scene.addBlock(blockWith(2, {1.0, 1.0, 1.0}));
    CHECK(notifications == 1);
}

TEST_CASE("scene storage modes enforce their construction invariants",
          "[unit][scene][storage]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    const auto source = std::make_shared<FakeHierarchySource>();

    CHECK_THROWS_AS(
        pci::PointCloudScene(metadata, nullptr, rootPayload(), 1024),
        std::invalid_argument);
    CHECK_THROWS_AS(pci::PointCloudScene(metadata, source, nullptr, 1024),
                    std::invalid_argument);

    pci::PointCloudScene flat(metadata);
    CHECK_FALSE(flat.hierarchical());
    CHECK_FALSE(flat.fullDetailInfo());
    CHECK_FALSE(flat.nodePayload(pci::rootPointCloudNode));
    CHECK(flat.decodedByteBudget() == 0);
    CHECK(flat.hierarchyError().empty());
    CHECK(flat.hierarchyMetrics().decodeRequestsQueued == 0);

    pci::PointCloudScene hierarchical(
        metadata, source, rootPayload(), 1024 * 1024);
    CHECK(hierarchical.hierarchical());
    CHECK(hierarchical.blockEntries().empty());
    CHECK_THROWS_AS(hierarchical.addBlock(blockWith(1, {0.0, 0.0, 0.0})),
                    std::logic_error);
}

TEST_CASE("hierarchical scenes decode requested nodes in the background",
          "[unit][scene][hierarchy]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto source = std::make_shared<FakeHierarchySource>();
    pci::PointCloudScene scene(metadata, source, rootPayload(), 1024 * 1024);
    const pci::PointCloudNodeId child =
        pci::childNodeId(pci::rootPointCloudNode, 0);

    scene.requestNodes(std::array{child});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    pci::PointCloudNodePayloadPtr decoded;
    while (!decoded && std::chrono::steady_clock::now() < deadline) {
        decoded = scene.nodePayload(child);
        std::this_thread::yield();
    }

    REQUIRE(decoded);
    CHECK(pci::pointCloudNodePayloadPoints(*decoded) == 4);
    CHECK(scene.totalPointCount() == 8);
    CHECK(scene.decodedResidentPoints() == 5);
    CHECK(scene.intensityMaximum() == 900);
    CHECK(scene.revision() == 2);
    const pci::PointCloudSceneMetrics metrics = scene.hierarchyMetrics();
    CHECK(metrics.decodeRequestsQueued == 1);
    CHECK(metrics.decodeRequestsStarted == 1);
    CHECK(metrics.decodeRequestsCompleted == 1);
    CHECK(metrics.decodeRequestsCancelled == 0);
    CHECK(metrics.decodeRequestsFailed == 0);
    CHECK(metrics.cache.insertions == 2);
    CHECK(metrics.cache.hits > 0);
}

TEST_CASE("hierarchical scenes cancel obsolete camera requests",
          "[unit][scene][hierarchy][cancellation]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto source = std::make_shared<CancellableHierarchySource>();
    pci::PointCloudScene scene(metadata, source, rootPayload(), 1024 * 1024);
    const pci::PointCloudNodeId obsolete =
        pci::childNodeId(pci::rootPointCloudNode, 0);
    const pci::PointCloudNodeId replacement =
        pci::childNodeId(pci::rootPointCloudNode, 1);

    scene.requestNodes(std::array{obsolete});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!source->obsoleteStarted &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(source->obsoleteStarted);

    scene.requestNodes(std::array{replacement});
    const auto replacementDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    pci::PointCloudNodePayloadPtr decoded;
    while (!decoded && std::chrono::steady_clock::now() < replacementDeadline) {
        decoded = scene.nodePayload(replacement);
        std::this_thread::yield();
    }

    REQUIRE(decoded);
    CHECK(source->obsoleteCancelled);
    CHECK(source->replacementLoaded);
    CHECK_FALSE(scene.nodePayload(obsolete));
}

TEST_CASE("newly pinned pages survive an already requested cancellation",
          "[unit][scene][hierarchy][cancellation][pin]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto source = std::make_shared<GatedHierarchySource>(true);
    pci::PointCloudScene scene(metadata, source, rootPayload(), 1024 * 1024);
    const pci::PointCloudNodeId child =
        pci::childNodeId(pci::rootPointCloudNode, 0);

    scene.requestNodes(std::array{child});
    const auto startDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!source->started &&
           std::chrono::steady_clock::now() < startDeadline) {
        std::this_thread::yield();
    }
    REQUIRE(source->started);

    scene.requestNodes({});
    scene.setPinnedNodes(std::array{child});
    source->released = true;

    const auto decodeDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    pci::PointCloudNodePayloadPtr decoded;
    while (!decoded && std::chrono::steady_clock::now() < decodeDeadline) {
        decoded = scene.nodePayload(child);
        std::this_thread::yield();
    }
    REQUIRE(decoded);
    const pci::PointCloudSceneMetrics metrics = scene.hierarchyMetrics();
    CHECK(metrics.decodeRequestsCancelled == 1);
    CHECK(metrics.decodeRequestsCompleted == 1);
    CHECK(metrics.decodeRequestsFailed == 0);
}

TEST_CASE("hierarchical scenes reload nodes after CPU eviction",
          "[unit][scene][hierarchy][cache]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 16;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto source = std::make_shared<FakeHierarchySource>();
    auto root = rootPayload();
    auto childPrototype = std::make_shared<pci::PointCloudNodePayload>();
    childPrototype->nodeId = pci::childNodeId(pci::rootPointCloudNode, 0);
    childPrototype->blocks.push_back(blockWith(4, {0.0, 0.0, 0.0}));
    const std::uint64_t budget =
        pci::pointCloudNodePayloadBytes(*root) +
        pci::pointCloudNodePayloadBytes(*childPrototype);
    pci::PointCloudScene scene(metadata, source, root, budget);
    root.reset();
    childPrototype.reset();
    const pci::PointCloudNodeId first =
        pci::childNodeId(pci::rootPointCloudNode, 0);
    const pci::PointCloudNodeId second =
        pci::childNodeId(pci::rootPointCloudNode, 1);
    const auto waitForNode = [&scene](const pci::PointCloudNodeId id) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        pci::PointCloudNodePayloadPtr value;
        while (!value && std::chrono::steady_clock::now() < deadline) {
            value = scene.nodePayload(id);
            std::this_thread::yield();
        }
        return value;
    };

    scene.requestNodes(std::array{first});
    auto firstLease = waitForNode(first);
    REQUIRE(firstLease);
    firstLease.reset();
    // A persistent renderer snapshot contains hierarchy metadata only. Keep
    // it alive while forcing eviction to prove that it does not retain the
    // decoded node payload.
    const pci::PointCloudSceneSnapshot rendererSnapshot = scene.snapshot();
    REQUIRE(rendererSnapshot.hierarchical);
    CHECK(rendererSnapshot.flatBlocks.empty());
    scene.requestNodes(std::array{second});
    auto secondLease = waitForNode(second);
    REQUIRE(secondLease);
    secondLease.reset();
    scene.trimDecodedCache();
    CHECK_FALSE(scene.nodePayload(first));

    scene.requestNodes(std::array{first});
    REQUIRE(waitForNode(first));
    CHECK(source->loadCount == 3);
    CHECK(scene.decodedResidentBytes() <= budget);
}

TEST_CASE("pinned hierarchy nodes survive camera-request cache churn",
          "[unit][scene][hierarchy][cache][pin]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 16;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto source = std::make_shared<FakeHierarchySource>();
    auto root = rootPayload();
    auto childPrototype = std::make_shared<pci::PointCloudNodePayload>();
    childPrototype->nodeId = pci::childNodeId(pci::rootPointCloudNode, 0);
    childPrototype->blocks.push_back(blockWith(4, {0.0, 0.0, 0.0}));
    const std::uint64_t budget =
        pci::pointCloudNodePayloadBytes(*root) +
        pci::pointCloudNodePayloadBytes(*childPrototype);
    pci::PointCloudScene scene(metadata, source, root, budget);
    root.reset();
    childPrototype.reset();

    const pci::PointCloudNodeId pinned =
        pci::childNodeId(pci::rootPointCloudNode, 0);
    const pci::PointCloudNodeId cameraRequest =
        pci::childNodeId(pci::rootPointCloudNode, 1);
    const auto waitForNode = [&scene](const pci::PointCloudNodeId id) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        pci::PointCloudNodePayloadPtr value;
        while (!value && std::chrono::steady_clock::now() < deadline) {
            value = scene.nodePayload(id);
            std::this_thread::yield();
        }
        return value;
    };

    scene.setPinnedNodes(std::array{pinned});
    auto pinnedLease = waitForNode(pinned);
    REQUIRE(pinnedLease);
    pinnedLease.reset();

    scene.requestNodes(std::array{cameraRequest});
    auto cameraLease = waitForNode(cameraRequest);
    REQUIRE(cameraLease);
    cameraLease.reset();
    scene.trimDecodedCache();

    CHECK(scene.nodePayload(pinned));
    CHECK_FALSE(scene.nodePayload(cameraRequest));
    CHECK(scene.decodedResidentBytes() <= budget);
    CHECK(source->loadCount == 2);
}

TEST_CASE("hierarchy cache churn stays bounded across a 20x working set",
          "[unit][scene][hierarchy][cache][stress]")
{
    constexpr std::size_t pointsPerNode = 64;
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = pointsPerNode * 64;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    const auto source =
        std::make_shared<StressHierarchySource>(std::uint8_t{2}, pointsPerNode);
    auto root = rootPayload();
    auto prototype = std::make_shared<pci::PointCloudNodePayload>();
    prototype->nodeId = {2, 0, 0, 0};
    prototype->blocks.push_back(blockWith(pointsPerNode, {-1.0, -1.0, -1.0}));
    const std::uint64_t payloadBytes =
        pci::pointCloudNodePayloadBytes(*prototype);
    const std::uint64_t budget =
        pci::pointCloudNodePayloadBytes(*root) + payloadBytes * 2;
    pci::PointCloudScene scene(metadata, source, root, budget);
    root.reset();
    prototype.reset();

    const auto waitForNode = [&scene](const pci::PointCloudNodeId id) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        pci::PointCloudNodePayloadPtr value;
        while (!value && std::chrono::steady_clock::now() < deadline) {
            value = scene.nodePayload(id);
            std::this_thread::yield();
        }
        return value;
    };

    std::vector<pci::PointCloudNodeId> workingSet;
    for (std::uint32_t z = 0; z < 4; ++z) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                workingSet.push_back({2, x, y, z});
            }
        }
    }
    REQUIRE(workingSet.size() == 64);
    CHECK(payloadBytes * workingSet.size() > budget * 20);

    for (const pci::PointCloudNodeId id : workingSet) {
        scene.requestNodes(std::array{id});
        auto lease = waitForNode(id);
        REQUIRE(lease);
        lease.reset();
        scene.trimDecodedCache();
        CHECK(scene.decodedResidentBytes() <= budget);
    }

    const pci::PointCloudNodeId revisit = workingSet.front();
    CHECK_FALSE(scene.nodePayload(revisit));
    scene.requestNodes(std::array{revisit});
    REQUIRE(waitForNode(revisit));
    CHECK(source->loadCount(revisit) == 2);

    const pci::PointCloudSceneMetrics metrics = scene.hierarchyMetrics();
    CHECK(metrics.cache.residentBytes <= metrics.cache.byteBudget);
    CHECK(metrics.cache.peakResidentBytes <= budget + payloadBytes);
    CHECK(metrics.cache.evictions >= workingSet.size() - 2);
    CHECK(metrics.source.completed == workingSet.size() + 1);
    CHECK(metrics.source.decodedBytesProduced > budget * 20);
    CHECK(metrics.cache.misses > 0);
}

TEST_CASE("rapid hierarchy request churn cancels obsolete decodes promptly",
          "[unit][scene][hierarchy][cancellation][stress]")
{
    using namespace std::chrono_literals;
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 4096;
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    const auto source = std::make_shared<StressHierarchySource>(
        std::uint8_t{2}, std::size_t{64}, 100ms);
    pci::PointCloudScene scene(metadata, source, rootPayload(), 1024 * 1024);

    std::vector<pci::PointCloudNodeId> requests;
    requests.reserve(16);
    for (std::uint32_t index = 0; index < 16; ++index) {
        requests.push_back({
            2,
            index & 3U,
            (index >> 2U) & 3U,
            0,
        });
    }
    scene.requestNodes(std::array{requests.front()});
    const auto waitForSourceRequests = [&source](const std::uint64_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (source->metrics().requests < count &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return source->metrics().requests >= count;
    };
    REQUIRE(waitForSourceRequests(1));

    std::chrono::milliseconds maximumCancellationLatency{};
    for (std::size_t index = 1; index < requests.size(); ++index) {
        const auto started = std::chrono::steady_clock::now();
        scene.requestNodes(std::array{requests[index]});
        REQUIRE(waitForSourceRequests(index + 1));
        maximumCancellationLatency =
            std::max(maximumCancellationLatency,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started));
    }

    const auto finalDeadline = std::chrono::steady_clock::now() + 2s;
    pci::PointCloudNodePayloadPtr finalPayload;
    while (!finalPayload && std::chrono::steady_clock::now() < finalDeadline) {
        finalPayload = scene.nodePayload(requests.back());
        std::this_thread::yield();
    }
    REQUIRE(finalPayload);
    CHECK(maximumCancellationLatency < 250ms);
    const pci::PointCloudSceneMetrics metrics = scene.hierarchyMetrics();
    CHECK(metrics.source.requests == requests.size());
    CHECK(metrics.source.cancelled == requests.size() - 1);
    CHECK(metrics.source.completed == 1);
    CHECK(metrics.decodeRequestsCancelled == requests.size() - 1);
    CHECK(metrics.decodeRequestsFailed == 0);
}

TEST_CASE("document bounds decode concurrency across hierarchical layers",
          "[unit][scene][hierarchy][residency][concurrency]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 8;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {2.0, 2.0, 2.0},
    };
    const auto firstSource = std::make_shared<GatedHierarchySource>(true);
    const auto secondSource = std::make_shared<GatedHierarchySource>(false);
    auto firstScene = std::make_shared<pci::PointCloudScene>(
        metadata, firstSource, rootPayload(), 1024 * 1024);
    auto secondScene = std::make_shared<pci::PointCloudScene>(
        metadata, secondSource, rootPayload(), 1024 * 1024);
    pci::SceneDocument document(1024 * 1024, 1);
    static_cast<void>(document.addLayer(firstScene));
    static_cast<void>(document.addLayer(secondScene));
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
    while (document.hierarchyScheduler()->metrics().pending == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    CHECK(document.hierarchyScheduler()->metrics().active == 1);
    REQUIRE(document.hierarchyScheduler()->metrics().pending == 1);
    CHECK(document.residencyCoordinator()->activeDecodeCount() == 0);
    CHECK(document.residencyCoordinator()->pendingDecodeCount() == 0);
    CHECK_FALSE(secondSource->started);

    firstSource->released = true;
    pci::PointCloudNodePayloadPtr secondPayload;
    while (!secondPayload && std::chrono::steady_clock::now() < deadline) {
        secondPayload = secondScene->nodePayload(child);
        std::this_thread::yield();
    }
    REQUIRE(secondPayload);
    CHECK(secondSource->started);
}

} // namespace
