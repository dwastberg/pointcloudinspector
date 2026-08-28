#include "renderer/planning/PointFrameCoordinator.h"
#include "renderer/planning/RenderSelection.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

constexpr pci::Bounds3d rootBounds{
    .minimum = {-1.0, -1.0, -1.0},
    .maximum = {1.0, 1.0, 1.0},
};

pci::PointCloudNode descriptor(const pci::PointCloudNodeId id)
{
    return {
        .id = id,
        .bounds = pci::pointCloudNodeBounds(rootBounds, id),
        .geometricError = id.level == 0 ? 1.0 : 0.0,
        .estimatedPointCount = id.level == 0 ? 10U : 5U,
        .leaf = id.level != 0,
    };
}

pci::RenderSelectionParameters parameters(const std::uint64_t budget = 100)
{
    return {
        .eye = {0.0, 0.0, 2.0},
        .verticalFieldOfViewDegrees = 60.0,
        .viewportHeight = 1080,
        .pointBudget = budget,
    };
}

TEST_CASE("LOD selection keeps a coarse parent until all children draw",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            return pci::RenderSelectionNodeState{
                .node = descriptor(id),
                .resident = id.level == 0,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [](const pci::Bounds3d &) {
            return true;
        },
        parameters());

    CHECK(result.drawNodes == std::vector{pci::rootPointCloudNode});
    CHECK(result.requestedNodes.size() == 8);
    CHECK(result.selectedPoints == 10);
}

TEST_CASE("LOD selection atomically replaces a parent with ready children",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            return pci::RenderSelectionNodeState{
                .node = descriptor(id),
                .resident = true,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [](const pci::Bounds3d &) {
            return true;
        },
        parameters());

    const auto children = pci::childNodeIds(pci::rootPointCloudNode);
    CHECK(result.drawNodes == std::vector(children.begin(), children.end()));
    CHECK(result.selectedPoints == 40);
}

TEST_CASE("LOD selection respects the point budget before refining",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            return pci::RenderSelectionNodeState{
                .node = descriptor(id),
                .resident = true,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [](const pci::Bounds3d &) {
            return true;
        },
        parameters(20));

    CHECK(result.drawNodes == std::vector{pci::rootPointCloudNode});
    CHECK(result.selectedPoints == 10);
    CHECK(result.requestedNodes.empty());
}

TEST_CASE("LOD selection requests only visible child regions",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            return pci::RenderSelectionNodeState{
                .node = descriptor(id),
                .resident = id.level == 0,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [](const pci::Bounds3d &bounds) {
            return bounds.minimum[0] < 0.0;
        },
        parameters());

    CHECK(result.drawNodes == std::vector{pci::rootPointCloudNode});
    CHECK(result.requestedNodes.size() == 4);
    CHECK(std::ranges::all_of(result.requestedNodes,
                              [](const pci::PointCloudNodeId id) {
                                  return id.x == 0;
                              }));
}

TEST_CASE("LOD selection ignores children known not to exist",
          "[unit][renderer-planning][lod][hierarchy-contract]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto children = pci::childNodeIds(pci::rootPointCloudNode);
    std::size_t visibilityTests = 0;
    const auto result = selector.select(
        roots,
        [&children](const pci::PointCloudNodeId id) {
            pci::PointCloudNode node = descriptor(id);
            const bool exists =
                id.level == 0 || id == children[0] || id == children[1];
            if (!exists) {
                node.bounds = {
                    .minimum = {1.0, 1.0, 1.0},
                    .maximum = {0.0, 0.0, 0.0},
                };
                node.estimatedPointCount = 0;
            }
            return pci::RenderSelectionNodeState{
                .node = node,
                .resident = exists,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [&visibilityTests](const pci::Bounds3d &) {
            ++visibilityTests;
            return true;
        },
        parameters());

    const std::vector<pci::PointCloudNodeId> existing(children.begin(),
                                                      children.begin() + 2);
    CHECK(result.drawNodes == existing);
    CHECK(result.requestedNodes == existing);
    CHECK(result.selectedPoints == 10);
    // Root once, then each existing child during filtering and traversal.
    // Known-nonexistent children never reach the visibility query.
    CHECK(visibilityTests == 5);
}

TEST_CASE("LOD selection stops at a served detail limit without claiming a "
          "leaf",
          "[unit][renderer-planning][lod][hierarchy-contract]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            pci::PointCloudNode node = descriptor(id);
            node.leaf = false;
            node.detailLimited = true;
            return pci::RenderSelectionNodeState{
                .node = node,
                .resident = true,
                .residentPointCount = 10,
            };
        },
        [](const pci::Bounds3d &) {
            return true;
        },
        parameters());

    CHECK(result.drawNodes == std::vector{pci::rootPointCloudNode});
    CHECK(result.requestedNodes.empty());
    CHECK(result.selectedPoints == 10);
}

TEST_CASE("LOD refinement hysteresis prevents threshold thrashing",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    const auto lookup = [](const pci::PointCloudNodeId id) {
        return pci::RenderSelectionNodeState{
            .node = descriptor(id),
            .resident = true,
            .residentPointCount = id.level == 0 ? 10U : 5U,
        };
    };
    const auto visible = [](const pci::Bounds3d &) {
        return true;
    };

    auto near = parameters();
    near.eye = {0.0, 0.0, 200.0};
    CHECK(selector.select(roots, lookup, visible, near).drawNodes.size() == 8);

    auto betweenThresholds = parameters();
    betweenThresholds.eye = {0.0, 0.0, 376.0};
    CHECK(selector.select(roots, lookup, visible, betweenThresholds)
              .drawNodes.size() == 8);

    auto far = parameters();
    far.eye = {0.0, 0.0, 550.0};
    CHECK(selector.select(roots, lookup, visible, far).drawNodes ==
          std::vector{pci::rootPointCloudNode});
}

TEST_CASE("LOD selection bounds pending detail requests",
          "[unit][renderer-planning][lod]")
{
    pci::RenderSelection selector;
    const std::array roots{pci::rootPointCloudNode};
    auto limited = parameters();
    limited.requestPointBudget = 12;
    const auto result = selector.select(
        roots,
        [](const pci::PointCloudNodeId id) {
            return pci::RenderSelectionNodeState{
                .node = descriptor(id),
                .resident = id.level == 0,
                .residentPointCount = id.level == 0 ? 10U : 5U,
            };
        },
        [](const pci::Bounds3d &) {
            return true;
        },
        limited);

    CHECK(result.drawNodes == std::vector{pci::rootPointCloudNode});
    CHECK(result.requestedNodes.size() == 2);
}

TEST_CASE("screen pick volume conservatively narrows spatial candidates",
          "[unit][renderer-planning][pick]")
{
    const pci::ScreenPickVolume pick =
        pci::makeScreenPickVolume({0.0, 0.0, 0.0},
                                  {0.0, 1.0, 0.0},
                                  {1.0, 0.0, 0.0},
                                  {0.0, 0.0, 1.0},
                                  60.0,
                                  1001,
                                  1001,
                                  500.0,
                                  500.0,
                                  4.0,
                                  0.1,
                                  100.0);
    REQUIRE(pick.valid());

    const pci::Bounds3d centred{
        .minimum = {-0.1, 5.0, -0.1},
        .maximum = {0.1, 6.0, 0.1},
    };
    const pci::Bounds3d outside{
        .minimum = {3.0, 5.0, -0.1},
        .maximum = {3.2, 6.0, 0.1},
    };
    const pci::Bounds3d behind{
        .minimum = {-0.1, -3.0, -0.1},
        .maximum = {0.1, -2.0, 0.1},
    };
    CHECK(pci::intersectsScreenPickVolume(pick, centred));
    CHECK_FALSE(pci::intersectsScreenPickVolume(pick, outside));
    CHECK_FALSE(pci::intersectsScreenPickVolume(pick, behind));
}

TEST_CASE("screen pick volume retains bounds touching its edge",
          "[unit][renderer-planning][pick]")
{
    const pci::ScreenPickVolume pick =
        pci::makeScreenPickVolume({0.0, 0.0, 0.0},
                                  {0.0, 1.0, 0.0},
                                  {1.0, 0.0, 0.0},
                                  {0.0, 0.0, 1.0},
                                  60.0,
                                  1000,
                                  1000,
                                  500.0,
                                  500.0,
                                  3.0,
                                  0.1,
                                  100.0);
    const double edge = 5.0 * pick.tangentHalfAngle;
    const pci::Bounds3d touching{
        .minimum = {edge, 5.0, 0.0},
        .maximum = {edge, 5.0, 0.0},
    };
    CHECK(pci::intersectsScreenPickVolume(pick, touching, 1e-9));
}

TEST_CASE("flat block planning distributes reduced detail stably",
          "[unit][renderer-planning][flat][budget]")
{
    const std::vector<pci::StableFlatBlockCandidate> candidates{
        {.groupId = 1, .blockId = 10, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 20, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 30, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 40, .pointCount = 100, .gpuBytes = 1600},
    };
    const auto plan = pci::planStableFlatBlocks(candidates, 6400, 200);
    REQUIRE(plan.size() == candidates.size());
    std::uint64_t points = 0;
    for (const pci::StableFlatBlockAllocation &allocation : plan) {
        CHECK(allocation.pointCount == 50);
        points += allocation.pointCount;
    }
    CHECK(points == 200);

    std::vector<pci::StableFlatBlockCandidate> reversed = candidates;
    std::ranges::reverse(reversed);
    const auto reversedPlan = pci::planStableFlatBlocks(reversed, 6400, 200);
    std::unordered_map<std::uint64_t, std::uint64_t> byBlock;
    for (const pci::StableFlatBlockAllocation &allocation : plan) {
        byBlock[candidates[allocation.candidateIndex].blockId] =
            allocation.pointCount;
    }
    for (const pci::StableFlatBlockAllocation &allocation : reversedPlan) {
        CHECK(byBlock.at(reversed[allocation.candidateIndex].blockId) ==
              allocation.pointCount);
    }
}

TEST_CASE("flat block planning respects complete-buffer residency",
          "[unit][renderer-planning][flat][budget]")
{
    const std::vector<pci::StableFlatBlockCandidate> candidates{
        {.groupId = 1, .blockId = 1, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 2, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 3, .pointCount = 100, .gpuBytes = 1600},
        {.groupId = 1, .blockId = 4, .pointCount = 100, .gpuBytes = 1600},
    };
    const auto plan = pci::planStableFlatBlocks(candidates, 3200, 1000);
    REQUIRE(plan.size() == 2);
    std::uint64_t bytes = 0;
    std::uint64_t points = 0;
    for (const pci::StableFlatBlockAllocation &allocation : plan) {
        bytes += candidates[allocation.candidateIndex].gpuBytes;
        points += allocation.pointCount;
    }
    CHECK(bytes == 3200);
    CHECK(points == 200);

    const auto repeated = pci::planStableFlatBlocks(candidates, 3200, 1000);
    REQUIRE(repeated.size() == plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
        CHECK(repeated[index].candidateIndex == plan[index].candidateIndex);
        CHECK(repeated[index].pointCount == plan[index].pointCount);
    }
}

TEST_CASE("global planning gives every source coverage before detail",
          "[unit][renderer-planning][planner][fairness]")
{
    std::vector<pci::LayerPointBudgetCandidate> candidates;
    candidates.reserve(25);
    for (std::uint64_t layer = 0; layer < 25; ++layer) {
        candidates.push_back({
            .layerId = 100 + layer,
            .coveragePoints = 100,
            .desiredPoints = 1'000,
            .projectedContribution = 1.0,
        });
    }

    const auto allocations = pci::planLayerPointBudgets(candidates, 1'000);
    REQUIRE(allocations.size() == candidates.size());
    for (const pci::LayerPointBudgetAllocation &allocation : allocations) {
        CHECK(allocation.pointBudget == 40);
    }
}

TEST_CASE("global planning is stable when source input order changes",
          "[unit][renderer-planning][planner][fairness]")
{
    std::vector<pci::LayerPointBudgetCandidate> candidates;
    candidates.reserve(25);
    for (std::uint64_t layer = 0; layer < 25; ++layer) {
        candidates.push_back({
            .layerId = 1'000 + layer,
            .coveragePoints = 20 + layer,
            .desiredPoints = 1'000 + 10 * layer,
            .projectedContribution = 1.0 + static_cast<double>(layer),
        });
    }
    const auto byLayer = [](const auto &input, const auto &plan) {
        std::unordered_map<std::uint64_t, std::uint64_t> result;
        for (const pci::LayerPointBudgetAllocation &allocation : plan) {
            result[input[allocation.candidateIndex].layerId] =
                allocation.pointBudget;
        }
        return result;
    };

    const auto first = pci::planLayerPointBudgets(candidates, 5'000);
    const auto expected = byLayer(candidates, first);
    std::ranges::reverse(candidates);
    const auto reordered = pci::planLayerPointBudgets(candidates, 5'000);
    CHECK(byLayer(candidates, reordered) == expected);
}

TEST_CASE("global detail follows projected contribution after coverage",
          "[unit][renderer-planning][planner][detail]")
{
    const std::array candidates{
        pci::LayerPointBudgetCandidate{
            .layerId = 1,
            .coveragePoints = 10,
            .desiredPoints = 100,
            .projectedContribution = 9.0,
        },
        pci::LayerPointBudgetCandidate{
            .layerId = 2,
            .coveragePoints = 10,
            .desiredPoints = 100,
            .projectedContribution = 1.0,
        },
    };
    const auto plan = pci::planLayerPointBudgets(candidates, 100);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].pointBudget == 82);
    CHECK(plan[1].pointBudget == 18);

    const pci::Bounds3d nearby{
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    const pci::Bounds3d distant{
        .minimum = {-1.0, -101.0, -1.0},
        .maximum = {1.0, -99.0, 1.0},
    };
    CHECK(pci::projectedBoundsContribution(nearby, {0.0, -5.0, 0.0}) >
          pci::projectedBoundsContribution(distant, {0.0, -5.0, 0.0}));
}

TEST_CASE("flat planning reserves GPU coverage for every source",
          "[unit][renderer-planning][flat][planner][fairness]")
{
    std::vector<pci::StableFlatBlockCandidate> candidates;
    for (std::uint64_t layer = 1; layer <= 25; ++layer) {
        candidates.push_back({
            .groupId = layer,
            .blockId = 1,
            .pointCount = 100,
            .gpuBytes = 1'600,
        });
        candidates.push_back({
            .groupId = layer,
            .blockId = 2,
            .pointCount = 100,
            .gpuBytes = 1'600,
        });
    }
    const auto plan = pci::planStableFlatBlocks(candidates, 25 * 1'600, 25);
    REQUIRE(plan.size() == 25);
    std::unordered_map<std::uint64_t, std::uint64_t> pointsByLayer;
    for (const pci::StableFlatBlockAllocation &allocation : plan) {
        pointsByLayer[candidates[allocation.candidateIndex].groupId] +=
            allocation.pointCount;
    }
    REQUIRE(pointsByLayer.size() == 25);
    CHECK(std::ranges::all_of(pointsByLayer, [](const auto &entry) {
        return entry.second == 1;
    }));
}

TEST_CASE("qualification-shaped flat frame plans match the golden fixture",
          "[unit][renderer-planning][golden][flat]")
{
    // Portable synthetic qualification recipe, seed 0x46504349 ("FPCI").
    // The IDs are fixed explicitly so the fixture is independent of local
    // qualification files, allocator order, and GPU availability.
    const std::vector<pci::FlatFrameBlockCandidate> candidates{
        {.layerId = 10, .blockId = 101, .pointCount = 10, .gpuBytes = 160},
        {.layerId = 20, .blockId = 201, .pointCount = 10, .gpuBytes = 160},
        {.layerId = 30, .blockId = 301, .pointCount = 10, .gpuBytes = 160},
        {.layerId = 40,
         .blockId = 401,
         .pointCount = 10,
         .gpuBytes = 160,
         .layerVisible = false},
        {.layerId = 50,
         .blockId = 501,
         .pointCount = 10,
         .gpuBytes = 160,
         .inFrustum = false},
        {.layerId = 60,
         .blockId = 601,
         .pointCount = 10,
         .gpuBytes = 160,
         .cpuResident = false},
    };
    const pci::FlatFramePlan plan =
        pci::PointFrameCoordinator::planFlatCandidates(candidates, 480, 7);

    CHECK(plan.selected == std::vector<pci::FlatFrameBlockSelection>{
                               {.candidateIndex = 0, .pointCount = 3},
                               {.candidateIndex = 1, .pointCount = 2},
                               {.candidateIndex = 2, .pointCount = 2},
                           });
    CHECK(plan.uploads == std::vector<std::size_t>{0, 1, 2});
    CHECK(plan.protectedBlocks == plan.uploads);
    CHECK(plan.coverage == std::vector<pci::FlatFrameLayerCoverage>{
                               {.layerId = 10, .covered = true},
                               {.layerId = 20, .covered = true},
                               {.layerId = 30, .covered = true},
                               {.layerId = 60, .covered = false},
                           });
    CHECK(plan.visibleBlockCount == 4);
    CHECK(plan.culledBlockCount == 1);
    CHECK_FALSE(plan.requiresContinuation);
}

TEST_CASE("golden flat frame plan preserves 25-layer coverage",
          "[unit][renderer-planning][golden][fairness]")
{
    std::vector<pci::FlatFrameBlockCandidate> candidates;
    for (std::uint64_t layer = 1; layer <= 25; ++layer) {
        candidates.push_back({
            .layerId = layer,
            .blockId = 1'000 + layer,
            .pointCount = 100,
            .gpuBytes = 1'600,
        });
    }
    const pci::FlatFramePlan plan =
        pci::PointFrameCoordinator::planFlatCandidates(
            candidates, 25 * 1'600, 25);
    REQUIRE(plan.selected.size() == 25);
    REQUIRE(plan.coverage.size() == 25);
    CHECK(std::ranges::all_of(plan.selected, [](const auto &selection) {
        return selection.pointCount == 1;
    }));
    CHECK(std::ranges::all_of(plan.coverage, [](const auto &coverage) {
        return coverage.covered;
    }));
    CHECK(plan.uploads == plan.protectedBlocks);
}

TEST_CASE("golden flat frame plan exposes insufficient GPU residency",
          "[unit][renderer-planning][golden][budget]")
{
    const std::vector<pci::FlatFrameBlockCandidate> candidates{
        {.layerId = 1, .blockId = 1, .pointCount = 100, .gpuBytes = 1'600},
        {.layerId = 1, .blockId = 2, .pointCount = 100, .gpuBytes = 1'600},
        {.layerId = 2, .blockId = 3, .pointCount = 100, .gpuBytes = 1'600},
        {.layerId = 2, .blockId = 4, .pointCount = 100, .gpuBytes = 1'600},
    };
    const pci::FlatFramePlan plan =
        pci::PointFrameCoordinator::planFlatCandidates(
            candidates, 3'200, 1'000);
    REQUIRE(plan.selected.size() == 2);
    CHECK(plan.selected[0].candidateIndex == 1);
    CHECK(plan.selected[0].pointCount == 100);
    CHECK(plan.selected[1].candidateIndex == 2);
    CHECK(plan.selected[1].pointCount == 100);
    CHECK(plan.coverage == std::vector<pci::FlatFrameLayerCoverage>{
                               {.layerId = 1, .covered = true},
                               {.layerId = 2, .covered = true},
                           });
}

TEST_CASE("full-detail admission accounts for every visible layer",
          "[unit][renderer-planning][full-detail][budget]")
{
    const std::array candidates{
        pci::FullDetailResidencyCandidate{
            .sourcePointCount = 100,
            .detailPointCount = 100,
            .decodedBytes = 300,
            .decodedByteBudget = 600,
            .gpuBytes = 200,
        },
        pci::FullDetailResidencyCandidate{
            .sourcePointCount = 200,
            .detailPointCount = 200,
            .decodedBytes = 400,
            .decodedByteBudget = 600,
            .gpuBytes = 300,
        },
    };

    // One eighth of each budget is deliberately held back for hierarchy
    // parents, block containers, and uploads in transition.
    CHECK(pci::fullDetailResidencyFits(candidates, 800, 600));
    CHECK_FALSE(pci::fullDetailResidencyFits(candidates, 799, 600));
    CHECK_FALSE(pci::fullDetailResidencyFits(candidates, 800, 571));
}

TEST_CASE("full-detail admission rejects incomplete or unfair working sets",
          "[unit][renderer-planning][full-detail][budget]")
{
    pci::FullDetailResidencyCandidate candidate{
        .sourcePointCount = 100,
        .detailPointCount = 99,
        .decodedBytes = 300,
        .decodedByteBudget = 600,
        .gpuBytes = 200,
    };
    CHECK_FALSE(
        pci::fullDetailResidencyFits(std::array{candidate}, 1'000, 1'000));

    candidate.detailPointCount = candidate.sourcePointCount;
    candidate.decodedBytes = 526;
    CHECK_FALSE(
        pci::fullDetailResidencyFits(std::array{candidate}, 1'000, 1'000));

    candidate.decodedBytes = 525;
    CHECK(pci::fullDetailResidencyFits(std::array{candidate}, 1'000, 1'000));
    CHECK_FALSE(pci::fullDetailResidencyFits(
        std::span<const pci::FullDetailResidencyCandidate>{}, 1'000, 1'000));
}

} // namespace
