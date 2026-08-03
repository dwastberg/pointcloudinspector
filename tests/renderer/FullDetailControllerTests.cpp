#include "renderer/planning/FullDetailController.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct FakeLayer {
    pci::PointCloudLayerId id;
    std::vector<pci::PointCloudNodeId> leaves;
    std::unordered_map<pci::PointCloudNodeId,
                       pci::PointCloudNodePayloadPtr,
                       pci::PointCloudNodeIdHash>
        payloads;
    pci::PointCloudNodePayloadPtr root;
    std::vector<pci::PointCloudNodeId> pinned;
    std::vector<std::vector<pci::PointCloudNodeId>> requests;
    bool decodePending = false;
    std::string error;
    std::uint64_t pointCount = 0;
};

pci::PointCloudNodeId leaf(const std::uint32_t x)
{
    return {.level = 1, .x = x};
}

pci::PointCloudNodePayloadPtr payload(const pci::PointCloudNodeId node,
                                      const std::size_t points)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->bounds = {
        .minimum = {static_cast<double>(node.x), 0.0, 0.0},
        .maximum = {static_cast<double>(node.x + 1U), 1.0, 1.0},
    };
    block->points.resize(points);
    auto result = std::make_shared<pci::PointCloudNodePayload>();
    result->nodeId = node;
    result->blocks.push_back(std::move(block));
    result->sourcePointCount = points;
    return result;
}

std::shared_ptr<FakeLayer> makeLayer(const std::uint64_t id,
                                     const std::size_t leafCount = 2)
{
    auto layer = std::make_shared<FakeLayer>();
    layer->id = pci::PointCloudLayerId{id};
    layer->root = payload(pci::rootPointCloudNode, 2);
    for (std::size_t index = 0; index < leafCount; ++index) {
        const pci::PointCloudNodeId node =
            leaf(static_cast<std::uint32_t>(index));
        layer->leaves.push_back(node);
        layer->pointCount += 3;
    }
    return layer;
}

pci::FullDetailLayerDescriptor
descriptor(const std::shared_ptr<FakeLayer> &layer,
           const bool loadingComplete = true,
           const bool exposeDetail = true)
{
    return {
        .layerId = layer->id,
        .hierarchical = true,
        .loadingComplete = loadingComplete,
        .sourcePointCount = layer->pointCount,
        .decodedByteBudget = 1'000'000,
        .detail = exposeDetail ? std::optional<pci::PointCloudFullDetailInfo>({
                                     .leafNodes = layer->leaves,
                                     .pointCount = layer->pointCount,
                                     .decodedBytes = 1'000,
                                     .gpuBytes = 1'000,
                                 })
                               : std::nullopt,
        .setPinnedNodes =
            [layer](const std::span<const pci::PointCloudNodeId> nodes) {
                layer->pinned.assign(nodes.begin(), nodes.end());
            },
        .peekPayload =
            [layer](const pci::PointCloudNodeId node) {
                const auto found = layer->payloads.find(node);
                return found == layer->payloads.end() ? nullptr : found->second;
            },
        .rootPayload =
            [layer] {
                return layer->root;
            },
        .nodeBounds =
            [](const pci::PointCloudNodeId node) {
                return pci::Bounds3d{
                    .minimum = {static_cast<double>(node.x), 0.0, 0.0},
                    .maximum = {static_cast<double>(node.x + 1U), 1.0, 1.0},
                };
            },
        .decodeInFlight =
            [layer] {
                return layer->decodePending;
            },
        .hierarchyError =
            [layer] {
                return layer->error;
            },
    };
}

pci::FullDetailConfiguration
configuration(const std::uint64_t revision,
              std::vector<pci::FullDetailLayerDescriptor> layers,
              const std::uint64_t decodedBudget = 1'000'000,
              const std::uint64_t gpuBudget = 1'000'000)
{
    return {
        .documentRevision = revision,
        .decodedByteBudget = decodedBudget,
        .gpuByteBudget = gpuBudget,
        .visibleLayers = std::move(layers),
    };
}

TEST_CASE("full detail atomically cuts over after the complete working set is "
          "resident",
          "[unit][renderer-planning][full-detail]")
{
    auto layer = makeLayer(1);
    for (const pci::PointCloudNodeId node : layer->leaves) {
        layer->payloads.emplace(node, payload(node, 3));
    }
    pci::FullDetailController controller;
    REQUIRE(controller.configure(configuration(1, {descriptor(layer)}))
                .planStarted);
    CHECK(layer->pinned == layer->leaves);

    std::unordered_set<pci::PointCloudNodeId, pci::PointCloudNodeIdHash>
        residentNodes{pci::rootPointCloudNode};
    const auto isResident = [&residentNodes](const pci::FullDetailBlockId &id) {
        return residentNodes.contains(id.nodeId);
    };
    const auto warming = controller.advance(
        [](const pci::Bounds3d &) {
            return true;
        },
        isResident);
    CHECK_FALSE(warming.active);
    REQUIRE(warming.drawableBlocks.size() == 1);
    CHECK(warming.drawableBlocks.front().id.nodeId == pci::rootPointCloudNode);
    CHECK(warming.uploadRequests.size() == 3);
    const auto initialProgress = controller.progressUpdates(isResident);
    REQUIRE(initialProgress.size() == 1);
    CHECK(initialProgress.front().decoded == 6);
    CHECK(initialProgress.front().uploaded == 0);
    CHECK(controller.progressUpdates(isResident).empty());

    residentNodes.insert(layer->leaves.front());
    const auto partialProgress = controller.progressUpdates(isResident);
    REQUIRE(partialProgress.size() == 1);
    CHECK(partialProgress.front().uploaded == 3);
    residentNodes.insert(layer->leaves.back());
    const auto active = controller.advance(
        [](const pci::Bounds3d &) {
            return true;
        },
        isResident);
    CHECK(active.active);
    CHECK(active.drawableBlocks.size() == 2);
    CHECK(active.selectedPoints == 6);
    CHECK(controller.status().active);
}

TEST_CASE("full detail admission rejects insufficient CPU and GPU budgets",
          "[unit][renderer-planning][full-detail]")
{
    auto layer = makeLayer(1);
    pci::FullDetailController controller;
    CHECK_FALSE(
        controller
            .configure(configuration(1, {descriptor(layer)}, 1'000, 1'000'000))
            .planStarted);
    CHECK_FALSE(controller.hasPlan());
    CHECK_FALSE(
        controller
            .configure(configuration(2, {descriptor(layer)}, 1'000'000, 1'000))
            .planStarted);
    CHECK_FALSE(controller.hasPlan());
    CHECK(layer->pinned.empty());
}

TEST_CASE("full detail recovers missing pages and bounds retry failures",
          "[unit][renderer-planning][full-detail]")
{
    auto layer = makeLayer(1, 1);
    pci::FullDetailController controller;
    REQUIRE(controller.configure(configuration(1, {descriptor(layer)}))
                .planStarted);
    const auto neverResident = [](const pci::FullDetailBlockId &) {
        return false;
    };
    for (std::uint8_t attempt = 0;
         attempt < pci::FullDetailController::maximumRecoveryAttempts;
         ++attempt) {
        const auto frame = controller.advance(
            [](const pci::Bounds3d &) {
                return true;
            },
            neverResident);
        REQUIRE(frame.nodeRequests.size() == 1);
        CHECK(frame.nodeRequests.front().nodes == layer->leaves);
    }
    CHECK_THROWS_WITH(
        controller.advance(
            [](const pci::Bounds3d &) {
                return true;
            },
            neverResident),
        "full-detail residency could not recover 1 missing hierarchy page(s)");
}

TEST_CASE("full detail reconfigures pins for hidden layers and revisions",
          "[unit][renderer-planning][full-detail]")
{
    auto first = makeLayer(1);
    auto second = makeLayer(2);
    pci::FullDetailController controller;
    REQUIRE(controller
                .configure(
                    configuration(1, {descriptor(first), descriptor(second)}))
                .planStarted);
    CHECK_FALSE(first->pinned.empty());
    CHECK_FALSE(second->pinned.empty());

    const auto hidden =
        controller.configure(configuration(2, {descriptor(first)}));
    CHECK(hidden.planStarted);
    CHECK(hidden.planStopped);
    CHECK_FALSE(first->pinned.empty());
    CHECK(second->pinned.empty());

    auto incomplete = makeLayer(3);
    const auto pending = controller.configure(
        configuration(3, {descriptor(incomplete, false, false)}));
    CHECK(pending.planStopped);
    CHECK(controller.decisionPending());
    CHECK(first->pinned.empty());
}

TEST_CASE("full detail probing is bounded per frame",
          "[unit][renderer-planning][full-detail]")
{
    auto layer =
        makeLayer(1, pci::FullDetailController::maximumNodeProbesPerFrame + 1);
    std::size_t probes = 0;
    pci::FullDetailLayerDescriptor input = descriptor(layer);
    input.peekPayload = [&probes](pci::PointCloudNodeId) {
        ++probes;
        return pci::PointCloudNodePayloadPtr{};
    };
    pci::FullDetailController controller;
    REQUIRE(controller.configure(configuration(1, {input})).planStarted);
    const auto frame = controller.advance(
        [](const pci::Bounds3d &) {
            return true;
        },
        [](const pci::FullDetailBlockId &) {
            return false;
        });
    CHECK(probes == pci::FullDetailController::maximumNodeProbesPerFrame);
    CHECK(frame.requiresContinuation);
    CHECK(frame.nodeRequests.empty());
}

} // namespace
