#pragma once

#include <pci/foundation/Bounds3d.h>
#include <pci/foundation/Vec3d.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pci {

// A source-level working-set request. Coverage is the source's preferred
// coarse representation; desiredPoints is the useful visible working set.
// projectedContribution controls only the detail pass, never coverage.
struct LayerPointBudgetCandidate {
    std::uint64_t layerId = 0;
    std::uint64_t coveragePoints = 0;
    std::uint64_t desiredPoints = 0;
    double projectedContribution = 1.0;
};

struct LayerPointBudgetAllocation {
    std::size_t candidateIndex = 0;
    std::uint64_t pointBudget = 0;
};

struct FlatFrameBlockCandidate {
    std::uint64_t layerId = 0;
    std::uint64_t blockId = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t gpuBytes = 0;
    bool layerVisible = true;
    bool inFrustum = true;
    bool cpuResident = true;
};

struct FlatFrameBlockSelection {
    std::size_t candidateIndex = 0;
    std::uint64_t pointCount = 0;

    bool operator==(const FlatFrameBlockSelection &) const = default;
};

struct FlatFrameLayerCoverage {
    std::uint64_t layerId = 0;
    bool covered = false;

    bool operator==(const FlatFrameLayerCoverage &) const = default;
};

// Portable characterization seam for the current flat-scene frame policy.
// Candidate indices in every output refer to the original input span.
struct FlatFramePlan {
    std::vector<FlatFrameBlockSelection> selected;
    std::vector<std::size_t> uploads;
    std::vector<std::size_t> protectedBlocks;
    std::vector<FlatFrameLayerCoverage> coverage;
    std::uint64_t visibleBlockCount = 0;
    std::uint64_t culledBlockCount = 0;
    bool requiresContinuation = false;
};

[[nodiscard]] FlatFramePlan
planFlatFrame(std::span<const FlatFrameBlockCandidate> candidates,
              std::uint64_t gpuByteBudget,
              std::uint64_t pointBudget);

// Allocates coarse coverage with deterministic max-min fairness, then assigns
// detail by projected contribution. Results refer to the input indices, while
// all tie-breaking uses stable layer IDs so input order cannot starve a layer.
[[nodiscard]] std::vector<LayerPointBudgetAllocation>
planLayerPointBudgets(std::span<const LayerPointBudgetCandidate> candidates,
                      std::uint64_t pointBudget);

// A bounded, monotonic estimate of screen contribution. It is intentionally
// independent of document order and is suitable for relative detail weights.
[[nodiscard]] double projectedBoundsContribution(const Bounds3d &bounds,
                                                 Vec3d eye) noexcept;

} // namespace pci
