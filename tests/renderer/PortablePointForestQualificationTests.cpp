#include "renderer/planning/FramePlanner.h"
#include "renderer/planning/FrustumCuller.h"
#include "renderer/planning/RenderSelection.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint64_t interiorPoints = 16'384;
constexpr std::uint64_t leafPoints = 32'768;

struct SyntheticSource {
    std::uint64_t layerId = 0;
    pci::Bounds3d bounds;
    std::uint8_t maximumLevel = 0;
    std::uint32_t phantomDivisor = 0;
    std::uint32_t cpuMissingDivisor = 0;
    std::uint32_t gpuMissingDivisor = 0;
};

struct SourceFrameResult {
    std::vector<pci::PointCloudNodeId> drawn;
    std::vector<pci::PointCloudNodeId> requested;
    std::uint64_t selectedPoints = 0;
    std::size_t lookupCount = 0;
    std::size_t cpuMissingLookups = 0;
    std::size_t gpuMissingLookups = 0;
};

std::uint64_t nodeCode(const pci::PointCloudNodeId id) noexcept
{
    return (static_cast<std::uint64_t>(id.level) << 60U) ^
           (static_cast<std::uint64_t>(id.x) << 40U) ^
           (static_cast<std::uint64_t>(id.y) << 20U) ^ id.z;
}

bool divisibleNode(const pci::PointCloudNodeId id,
                   const std::uint32_t divisor,
                   const std::uint64_t salt) noexcept
{
    if (id == pci::rootPointCloudNode || divisor == 0) {
        return false;
    }
    const std::uint64_t mixed =
        nodeCode(id) * std::uint64_t{0x9e3779b97f4a7c15} + salt;
    return mixed % divisor == 0;
}

bool exists(const SyntheticSource &source,
            const pci::PointCloudNodeId id) noexcept
{
    return id.level <= source.maximumLevel &&
           !divisibleNode(id, source.phantomDivisor, source.layerId * 17U);
}

std::uint64_t representedNodeCount(const SyntheticSource &source)
{
    std::uint64_t result = 0;
    std::vector<pci::PointCloudNodeId> level{pci::rootPointCloudNode};
    while (!level.empty()) {
        std::vector<pci::PointCloudNodeId> next;
        for (const pci::PointCloudNodeId id : level) {
            if (!exists(source, id)) {
                continue;
            }
            ++result;
            if (id.level < source.maximumLevel) {
                const auto children = pci::childNodeIds(id);
                next.insert(next.end(), children.begin(), children.end());
            }
        }
        level = std::move(next);
    }
    return result;
}

std::vector<SyntheticSource> qualificationSources()
{
    return {
        {101,
         {.minimum = {-54.0, -45.0, -3.0},
          .maximum = {46.0, 55.0, 17.0}},
         4,
         17,
         13,
         19},
        {202,
         {.minimum = {-42.0, -52.0, -6.0},
          .maximum = {58.0, 48.0, 14.0}},
         5,
         19,
         17,
         23},
        {303,
         {.minimum = {-61.0, -36.0, -2.0},
          .maximum = {39.0, 64.0, 18.0}},
         4,
         23,
         11,
         29},
        {404,
         {.minimum = {-35.0, -60.0, -8.0},
          .maximum = {65.0, 40.0, 12.0}},
         5,
         29,
         19,
         13},
        {505,
         {.minimum = {-48.0, -40.0, -4.0},
          .maximum = {52.0, 60.0, 16.0}},
         3,
         13,
         23,
         17},
        {606,
         {.minimum = {-57.0, -57.0, -5.0},
          .maximum = {43.0, 43.0, 15.0}},
         5,
         31,
         29,
         11},
    };
}

pci::FrustumCuller topDownCamera(const double centerX,
                                 const double scale = 72.0)
{
    const pci::Vec3d eye{centerX, 0.0, 120.0};
    return pci::FrustumCuller::fromOrthographic(
        eye,
        {0.0, 0.0, -1.0},
        {0.0, 1.0, 0.0},
        {1.0, 0.0, 0.0},
        scale,
        16.0 / 10.0,
        0.1,
        300.0);
}

SourceFrameResult selectSource(pci::RenderSelection &selection,
                               const SyntheticSource &source,
                               const pci::FrustumCuller &camera,
                               const double eyeX,
                               const std::uint64_t pointBudget)
{
    SourceFrameResult result;
    std::unordered_map<pci::PointCloudNodeId,
                       std::size_t,
                       pci::PointCloudNodeIdHash>
        lookups;
    const pci::RenderSelectionResult selected = selection.select(
        std::array{pci::rootPointCloudNode},
        [&source, &result, &lookups](const pci::PointCloudNodeId id) {
            ++result.lookupCount;
            ++lookups[id];
            if (!exists(source, id)) {
                return pci::RenderSelectionNodeState{
                    .node = {.id = id},
                };
            }
            const bool cpuResident = !divisibleNode(
                id, source.cpuMissingDivisor, source.layerId * 31U);
            const bool gpuResident = !divisibleNode(
                id, source.gpuMissingDivisor, source.layerId * 43U);
            result.cpuMissingLookups += cpuResident ? 0U : 1U;
            result.gpuMissingLookups += gpuResident ? 0U : 1U;
            const bool leaf = id.level == source.maximumLevel;
            const pci::Bounds3d bounds =
                pci::pointCloudNodeBounds(source.bounds, id);
            return pci::RenderSelectionNodeState{
                .node =
                    {.id = id,
                     .bounds = bounds,
                     .geometricError =
                         bounds.maximumExtent() /
                         (leaf ? 181.0 : 128.0),
                     .estimatedPointCount = leaf ? leafPoints : interiorPoints,
                     .leaf = leaf},
                .resident = cpuResident && gpuResident,
                .residentPointCount = leaf ? leafPoints : interiorPoints,
            };
        },
        [&camera](const pci::Bounds3d &bounds) {
            return camera.intersects(bounds);
        },
        {.eye = {eyeX, 0.0, 120.0},
         .viewportHeight = 800,
         .orthographicScale = 72.0,
         .orthographic = true,
         .pointBudget = pointBudget,
         .requestPointBudget = 96'000});

    // Phase 2 guarantees one metadata lookup per visited tree node. This is
    // the portable operation bound: it catches accidental duplicate walks
    // without depending on machine timing.
    CHECK(std::ranges::all_of(lookups, [](const auto &entry) {
        return entry.second == 1;
    }));
    result.drawn = selected.drawNodes;
    result.requested = selected.requestedNodes;
    result.selectedPoints = selected.selectedPoints;
    return result;
}

std::unordered_map<std::uint64_t, std::uint64_t>
allocateLayerBudgets(const std::vector<SyntheticSource> &sources,
                     const pci::FrustumCuller &camera,
                     const double eyeX,
                     const std::uint64_t pointBudget)
{
    std::vector<pci::LayerPointBudgetCandidate> candidates;
    for (const SyntheticSource &source : sources) {
        if (!camera.intersects(source.bounds)) {
            continue;
        }
        candidates.push_back({
            .layerId = source.layerId,
            .coveragePoints = interiorPoints,
            .desiredPoints = leafPoints * 64U,
            .projectedContribution =
                pci::projectedBoundsContribution(source.bounds,
                                                 {eyeX, 0.0, 120.0}),
        });
    }
    std::unordered_map<std::uint64_t, std::uint64_t> result;
    for (const pci::LayerPointBudgetAllocation &allocation :
         pci::planLayerPointBudgets(candidates, pointBudget)) {
        result[candidates[allocation.candidateIndex].layerId] =
            allocation.pointBudget;
    }
    return result;
}

std::unordered_map<std::uint64_t, SourceFrameResult>
selectFrame(const std::vector<SyntheticSource> &sources,
            std::unordered_map<std::uint64_t, pci::RenderSelection> &selectors,
            const double eyeX,
            const std::uint64_t pointBudget)
{
    const pci::FrustumCuller camera = topDownCamera(eyeX);
    const auto budgets =
        allocateLayerBudgets(sources, camera, eyeX, pointBudget);
    std::unordered_map<std::uint64_t, SourceFrameResult> result;
    for (const SyntheticSource &source : sources) {
        const auto budget = budgets.find(source.layerId);
        if (budget == budgets.end()) {
            continue;
        }
        result.emplace(source.layerId,
                       selectSource(selectors[source.layerId],
                                    source,
                                    camera,
                                    eyeX,
                                    budget->second));
    }
    return result;
}

TEST_CASE("portable six-source forest qualification stays bounded and covered",
          "[qualification][renderer-planning][hierarchy][stress]")
{
    const std::vector<SyntheticSource> sources = qualificationSources();
    const std::uint64_t representedNodes =
        std::ranges::fold_left(sources, std::uint64_t{0}, [](const auto total,
                                                             const auto &source) {
            return total + representedNodeCount(source);
        });
    INFO("metadata-only nodes: " << representedNodes);
    REQUIRE(representedNodes > 10'000);

    // The path crosses several overlapping source seams, then dwells on the
    // same view. It is deliberately frame-indexed and contains no sleeps.
    constexpr std::array cameraPath{-34.0, -18.0, 0.0, 18.0, 34.0, 34.0};
    constexpr std::uint64_t framePointBudget = 393'216;
    std::unordered_map<std::uint64_t, pci::RenderSelection> selectors;
    std::size_t totalLookups = 0;
    bool observedCpuGap = false;
    bool observedGpuGap = false;
    for (const double eyeX : cameraPath) {
        const auto frame =
            selectFrame(sources, selectors, eyeX, framePointBudget);
        REQUIRE(frame.size() == 6);
        std::uint64_t submitted = 0;
        for (const auto &[layerId, source] : frame) {
            INFO("layer: " << layerId << ", camera x: " << eyeX);
            CHECK_FALSE(source.drawn.empty());
            CHECK(source.selectedPoints >= interiorPoints);
            submitted += source.selectedPoints;
            totalLookups += source.lookupCount;
            observedCpuGap = observedCpuGap || source.cpuMissingLookups > 0;
            observedGpuGap = observedGpuGap || source.gpuMissingLookups > 0;
            CHECK(source.requested.size() <= 8);
        }
        CHECK(submitted <= framePointBudget);
    }
    CHECK(observedCpuGap);
    CHECK(observedGpuGap);
    // Six frames * six sources * a root and at most eight children. Tightening
    // this bound is safe only when the traversal contract changes explicitly.
    CHECK(totalLookups <= cameraPath.size() * sources.size() * 9U);
}

TEST_CASE("portable forest qualification is independent of document order",
          "[qualification][renderer-planning][hierarchy][order]")
{
    const std::vector<SyntheticSource> forward = qualificationSources();
    std::vector<SyntheticSource> reversed = forward;
    std::ranges::reverse(reversed);
    std::unordered_map<std::uint64_t, pci::RenderSelection> forwardSelectors;
    std::unordered_map<std::uint64_t, pci::RenderSelection> reverseSelectors;

    const auto first =
        selectFrame(forward, forwardSelectors, 0.0, 393'216);
    const auto second =
        selectFrame(reversed, reverseSelectors, 0.0, 393'216);
    REQUIRE(first.size() == second.size());
    for (const auto &[layerId, source] : first) {
        const auto reordered = second.find(layerId);
        REQUIRE(reordered != second.end());
        CHECK(source.selectedPoints == reordered->second.selectedPoints);
        CHECK(source.drawn == reordered->second.drawn);
        CHECK(source.requested == reordered->second.requested);
    }
}

} // namespace
