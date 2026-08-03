#include "scene/DecodedPageCache.h"
#include "scene/HierarchyResidencyCoordinator.h"
#include "scene/PointCloudNode.h"
#include "scene/PointMemoryBudget.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

constexpr pci::PointCloudSourceId cacheSource{17};

pci::PointCloudNodePayloadPtr payload(const pci::PointCloudNodeId id,
                                      const std::size_t pointCount)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(pointCount);
    block->attributes.resize(pointCount);
    auto result = std::make_shared<pci::PointCloudNodePayload>();
    result->nodeId = id;
    result->blocks.push_back(std::move(block));
    result->sourcePointCount = pointCount;
    return result;
}

TEST_CASE("hierarchy composite hashes cover every key field",
          "[unit][hierarchy][hash]")
{
    const pci::PointCloudNodeId node{.level = 2, .x = 1, .y = 2, .z = 3};
    const pci::PointCloudNodeId same = node;
    const pci::PointCloudNodeId differentLevel{
        .level = 3, .x = 1, .y = 2, .z = 3};
    const pci::PointCloudNodeId differentX{.level = 2, .x = 4, .y = 2, .z = 3};
    const pci::PointCloudNodeId differentY{.level = 2, .x = 1, .y = 4, .z = 3};
    const pci::PointCloudNodeId differentZ{.level = 2, .x = 1, .y = 2, .z = 4};
    const pci::PointCloudNodeIdHash nodeHash;
    CHECK(nodeHash(node) == nodeHash(same));
    CHECK(nodeHash(node) != nodeHash(differentLevel));
    CHECK(nodeHash(node) != nodeHash(differentX));
    CHECK(nodeHash(node) != nodeHash(differentY));
    CHECK(nodeHash(node) != nodeHash(differentZ));

    const pci::DecodedPageKey page{.sourceId = cacheSource, .nodeId = node};
    const pci::DecodedPageKey samePage = page;
    const pci::DecodedPageKey differentSource{
        .sourceId = pci::PointCloudSourceId{cacheSource.value() + 1},
        .nodeId = node};
    const pci::DecodedPageKey differentNode{.sourceId = cacheSource,
                                            .nodeId = differentX};
    const pci::DecodedPageKeyHash pageHash;
    CHECK(pageHash(page) == pageHash(samePage));
    CHECK(pageHash(page) != pageHash(differentSource));
    CHECK(pageHash(page) != pageHash(differentNode));
}

TEST_CASE("octree node identifiers and bounds subdivide consistently",
          "[unit][hierarchy]")
{
    const pci::Bounds3d root{
        .minimum = {0.0, 10.0, 20.0},
        .maximum = {8.0, 18.0, 28.0},
    };
    const auto children = pci::childNodeIds(pci::rootPointCloudNode);

    CHECK(children[0] == pci::PointCloudNodeId{1, 0, 0, 0});
    CHECK(children[7] == pci::PointCloudNodeId{1, 1, 1, 1});
    CHECK(pci::pointCloudNodeBounds(root, children[0]).minimum ==
          std::array{0.0, 10.0, 20.0});
    CHECK(pci::pointCloudNodeBounds(root, children[0]).maximum ==
          std::array{4.0, 14.0, 24.0});
    CHECK(pci::pointCloudNodeBounds(root, children[7]).minimum ==
          std::array{4.0, 14.0, 24.0});
    CHECK(pci::pointCloudNodeBounds(root, children[7]).maximum == root.maximum);
}

TEST_CASE("decoded cache evicts the least recently used unleased node",
          "[unit][hierarchy][cache]")
{
    const pci::PointCloudNodeId first{1, 0, 0, 0};
    const pci::PointCloudNodeId second{1, 1, 0, 0};
    const pci::PointCloudNodeId third{1, 0, 1, 0};
    auto prototype = payload(first, 4);
    const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*prototype);
    pci::DecodedPageCache cache(bytes * 2);

    cache.insert(cacheSource, prototype);
    prototype.reset();
    cache.insert(cacheSource, payload(second, 4));
    auto recentlyUsed = cache.find(cacheSource, first);
    recentlyUsed.reset();
    cache.insert(cacheSource, payload(third, 4));
    CHECK_FALSE(cache.find(cacheSource, second));

    CHECK(cache.contains(cacheSource, first));
    CHECK_FALSE(cache.contains(cacheSource, second));
    CHECK(cache.contains(cacheSource, third));
    CHECK(cache.residentBytes() <= cache.byteBudget());
    CHECK(cache.residentPoints() == 8);
    const pci::DecodedCacheMetrics metrics = cache.metrics();
    CHECK(metrics.hits == 1);
    CHECK(metrics.misses == 1);
    CHECK(metrics.insertions == 3);
    CHECK(metrics.evictions == 1);
    CHECK(metrics.residentBytes == bytes * 2);
    CHECK(metrics.peakResidentBytes == bytes * 3);
}

TEST_CASE("decoded cache does not evict payloads leased by a frame",
          "[unit][hierarchy][cache]")
{
    const pci::PointCloudNodeId first{1, 0, 0, 0};
    const pci::PointCloudNodeId second{1, 1, 0, 0};
    auto firstPayload = payload(first, 4);
    const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*firstPayload);
    pci::DecodedPageCache cache(bytes);
    cache.insert(cacheSource, firstPayload);
    firstPayload.reset();

    const auto frameLease = cache.find(cacheSource, first);
    cache.insert(cacheSource, payload(second, 4));

    CHECK(cache.contains(cacheSource, first));
    CHECK_FALSE(cache.contains(cacheSource, second));
    CHECK(cache.residentBytes() == bytes);
}

TEST_CASE("decoded cache replaces pages without double counting",
          "[unit][hierarchy][cache]")
{
    const pci::PointCloudNodeId node{1, 0, 0, 0};
    auto original = payload(node, 4);
    const std::uint64_t originalBytes =
        pci::pointCloudNodePayloadBytes(*original);
    pci::DecodedPageCache cache(originalBytes * 2);
    cache.insert(cacheSource, original);

    auto replacement = payload(node, 2);
    const std::uint64_t replacementBytes =
        pci::pointCloudNodePayloadBytes(*replacement);
    cache.insert(cacheSource, replacement);

    CHECK(cache.find(cacheSource, node) == replacement);
    CHECK(cache.residentBytes() == replacementBytes);
    CHECK(cache.residentPoints() == 2);
    CHECK(cache.metrics().insertions == 2);
    CHECK(cache.metrics().replacements == 1);
}

TEST_CASE("decoded cache honors protected and pinned pages",
          "[unit][hierarchy][cache][pin]")
{
    const pci::PointCloudNodeId protectedNode{1, 0, 0, 0};
    const pci::PointCloudNodeId rejectedNode{1, 1, 0, 0};
    auto prototype = payload(protectedNode, 4);
    const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*prototype);
    pci::DecodedPageCache cache(bytes);
    cache.insert(cacheSource, prototype);
    prototype.reset();

    const std::array protectedNodes{protectedNode};
    cache.insert(cacheSource, payload(rejectedNode, 4), protectedNodes);
    CHECK(cache.contains(cacheSource, protectedNode));
    CHECK_FALSE(cache.contains(cacheSource, rejectedNode));

    cache.setPinned(cacheSource, protectedNodes);
    cache.insert(cacheSource, payload(rejectedNode, 4));
    CHECK(cache.contains(cacheSource, protectedNode));
    CHECK_FALSE(cache.contains(cacheSource, rejectedNode));
    CHECK(cache.metrics().evictions == 2);
}

TEST_CASE("document page cache applies one LRU across source identities",
          "[unit][hierarchy][cache][multi-layer]")
{
    const pci::PointCloudNodeId first{1, 0, 0, 0};
    const pci::PointCloudNodeId second{1, 1, 0, 0};
    const auto prototype = payload(first, 4);
    const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*prototype);
    pci::DecodedPageCache cache(bytes * 2);
    const pci::PointCloudSourceId source1{1};
    const pci::PointCloudSourceId source2{2};
    const pci::PointCloudSourceId source3{3};

    cache.insert(source1, prototype);
    cache.insert(source2, payload(first, 4));
    auto recentlyUsed = cache.find(source1, first);
    recentlyUsed.reset();
    cache.insert(source3, payload(second, 4));

    CHECK(cache.contains(source1, first));
    CHECK_FALSE(cache.contains(source2, first));
    CHECK(cache.contains(source3, second));
    CHECK(cache.residentBytes() <= cache.byteBudget());
    CHECK(cache.metrics().evictions == 1);
    CHECK(cache.metrics(source2).evictions == 1);
}

TEST_CASE("page cache completion probes do not distort hit metrics",
          "[unit][hierarchy][cache][metrics]")
{
    const pci::PointCloudNodeId node{1, 0, 0, 0};
    pci::DecodedPageCache cache(1024 * 1024);
    const pci::PointCloudSourceId source{1};
    cache.insert(source, payload(node, 4));

    CHECK(cache.peek(source, node));
    CHECK_FALSE(cache.peek(source, pci::PointCloudNodeId{1, 1, 0, 0}));
    CHECK(cache.metrics().hits == 0);
    CHECK(cache.metrics().misses == 0);

    CHECK(cache.find(source, node));
    CHECK_FALSE(cache.find(source, pci::PointCloudNodeId{1, 1, 0, 0}));
    CHECK(cache.metrics().hits == 1);
    CHECK(cache.metrics().misses == 1);
}

TEST_CASE("decoded cache validates its byte budget", "[unit][hierarchy][cache]")
{
    CHECK_THROWS_AS(pci::DecodedPageCache(0), std::invalid_argument);
    pci::DecodedPageCache cache(1);
    CHECK_THROWS_AS(cache.setByteBudget(0), std::invalid_argument);
}

TEST_CASE("decoded cache remains bounded under an oversized working set",
          "[unit][hierarchy][cache][stress]")
{
    auto prototype = payload({1, 0, 0, 0}, 64);
    const std::uint64_t bytes = pci::pointCloudNodePayloadBytes(*prototype);
    prototype.reset();
    pci::DecodedPageCache cache(bytes * 2);

    for (std::uint32_t index = 0; index < 32; ++index) {
        cache.insert(cacheSource, payload({5, index, 0, 0}, 64));
        CHECK(cache.residentBytes() <= cache.byteBudget());
        CHECK(cache.residentPoints() <= 128);
    }
    CHECK(cache.size() == 2);
}

TEST_CASE("hierarchy residency divides discretionary bytes fairly",
          "[unit][hierarchy][cache][residency]")
{
    auto coordinator =
        std::make_shared<pci::HierarchyResidencyCoordinator>(1000, 1);
    const auto first = coordinator->registerParticipant(100);
    const auto second = coordinator->registerParticipant(100);

    CHECK(first->byteBudget() == 500);
    CHECK(second->byteBudget() == 500);
    const auto transient = coordinator->registerTransientDecode();
    CHECK(first->byteBudget() == 500);
    CHECK(second->byteBudget() == 500);

    second->setActive(false);
    CHECK(first->byteBudget() == 900);
    CHECK(second->byteBudget() == 100);
    CHECK(first->byteBudget() + second->byteBudget() ==
          coordinator->byteBudget());
}

TEST_CASE("flat reservations reduce hierarchy discretionary residency",
          "[unit][hierarchy][cache][residency]")
{
    auto memory = std::make_shared<pci::PointMemoryBudget>(1000);
    auto flat = memory->tryReserve(200);
    REQUIRE(flat);
    auto coordinator = std::make_shared<pci::HierarchyResidencyCoordinator>(
        1000, 1, pci::HierarchyDecodeAdmissionPtr{}, memory);
    const auto first = coordinator->registerParticipant(100);
    const auto second = coordinator->registerParticipant(100);

    CHECK(first->byteBudget() == 400);
    CHECK(second->byteBudget() == 400);
    CHECK(first->byteBudget() + second->byteBudget() +
              memory->reservedBytes() ==
          memory->byteBudget());

    REQUIRE((*flat)->tryResize(100));
    CHECK(first->byteBudget() == 450);
    CHECK(second->byteBudget() == 450);
}

TEST_CASE("point memory reservations are atomic and release on lifetime",
          "[unit][residency][memory]")
{
    auto memory = std::make_shared<pci::PointMemoryBudget>(100);
    auto first = memory->tryReserve(60);
    REQUIRE(first);
    CHECK_FALSE(memory->tryReserve(41));
    CHECK(memory->reservedBytes() == 60);
    REQUIRE((*first)->tryResize(40));
    auto second = memory->tryReserve(60);
    REQUIRE(second);
    CHECK(memory->availableBytes() == 0);
    first.reset();
    CHECK(memory->reservedBytes() == 60);
    second.reset();
    CHECK(memory->reservedBytes() == 0);

    const pci::PointMemoryBudgetMetrics metrics = memory->metrics();
    CHECK(metrics.peakReservedBytes == 100);
    CHECK(metrics.reservationRequests == 3);
    CHECK(metrics.reservationFailures == 1);
}

TEST_CASE("point memory ceiling can follow live system headroom",
          "[unit][residency][memory]")
{
    auto memory = std::make_shared<pci::PointMemoryBudget>(100);
    auto reservation = memory->tryReserve(60);
    REQUIRE(reservation);

    CHECK_FALSE(memory->setByteBudget(59));
    CHECK(memory->byteBudget() == 100);
    REQUIRE(memory->setByteBudget(80));
    CHECK(memory->availableBytes() == 20);
    REQUIRE(memory->setByteBudget(200));
    CHECK(memory->availableBytes() == 140);
    CHECK_FALSE(memory->setByteBudget(0));
}

TEST_CASE("hierarchy decode admission is globally bounded and FIFO",
          "[unit][hierarchy][residency][concurrency]")
{
    using namespace std::chrono_literals;
    auto admission = std::make_shared<pci::HierarchyDecodeAdmission>(1);
    auto firstCoordinator =
        std::make_shared<pci::HierarchyResidencyCoordinator>(
            1024, 1, admission);
    auto secondCoordinator =
        std::make_shared<pci::HierarchyResidencyCoordinator>(
            2048, 1, admission);
    const auto first = firstCoordinator->registerParticipant(1);
    const auto second = secondCoordinator->registerParticipant(1);
    std::stop_source initialStop;
    auto initial = first->acquireDecode(
        initialStop.get_token(), initialStop.get_token(), 100);
    REQUIRE(initial);

    std::mutex orderMutex;
    std::vector<int> order;
    const auto waitForPending = [&admission](const std::size_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (admission->pendingDecodeCount() < expected &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return admission->pendingDecodeCount() >= expected;
    };
    const auto acquire = [&orderMutex,
                          &order](const auto &participant,
                                  const int marker,
                                  const std::stop_token stopToken) {
        const auto lease = participant->acquireDecode(
            stopToken, stopToken, static_cast<std::uint64_t>(marker) * 100U);
        if (lease) {
            const std::scoped_lock lock(orderMutex);
            order.push_back(marker);
        }
    };

    std::jthread secondWaiter(
        [&acquire, &second](const std::stop_token stopToken) {
            acquire(second, 2, stopToken);
        });
    REQUIRE(waitForPending(1));
    std::jthread firstWaiter(
        [&acquire, &first](const std::stop_token stopToken) {
            acquire(first, 1, stopToken);
        });
    REQUIRE(waitForPending(2));

    initial.reset();
    secondWaiter.join();
    firstWaiter.join();

    CHECK(order == std::vector{2, 1});
    CHECK(admission->activeDecodeCount() == 0);
    CHECK(admission->pendingDecodeCount() == 0);
    const pci::HierarchyDecodeAdmissionMetrics metrics = admission->metrics();
    CHECK(metrics.requests == 3);
    CHECK(metrics.admitted == 3);
    CHECK(metrics.cancelled == 0);
    CHECK(metrics.peakActive == 1);
    CHECK(metrics.peakPending == 2);
    CHECK(metrics.peakActiveEstimatedBytes == 200);
    CHECK(metrics.activeEstimatedBytes == 0);
}

} // namespace
