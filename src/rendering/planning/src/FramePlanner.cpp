#include <pci/rendering/planning/FramePlanner.h>

#include <pci/foundation/CheckedArithmetic.h>
#include <pci/rendering/planning/RenderSelection.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pci {
namespace {

double usableWeight(const LayerPointBudgetCandidate &candidate) noexcept
{
    return std::isfinite(candidate.projectedContribution) &&
                   candidate.projectedContribution > 0.0
               ? std::clamp(candidate.projectedContribution, 1e-12, 1e12)
               : 1.0;
}

} // namespace

FlatFramePlan
planFlatFrame(const std::span<const FlatFrameBlockCandidate> candidates,
              const std::uint64_t gpuByteBudget,
              const std::uint64_t pointBudget)
{
    FlatFramePlan result;
    std::vector<StableFlatBlockCandidate> resident;
    std::vector<std::size_t> originalIndices;
    std::unordered_map<std::uint64_t, bool> coverage;
    resident.reserve(candidates.size());
    originalIndices.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const FlatFrameBlockCandidate &candidate = candidates[index];
        if (!candidate.layerVisible) {
            continue;
        }
        if (!candidate.inFrustum) {
            ++result.culledBlockCount;
            continue;
        }
        ++result.visibleBlockCount;
        coverage.try_emplace(candidate.layerId, false);
        if (!candidate.cpuResident) {
            continue;
        }
        resident.push_back({
            .groupId = candidate.layerId,
            .blockId = candidate.blockId,
            .pointCount = candidate.pointCount,
            .gpuBytes = candidate.gpuBytes,
        });
        originalIndices.push_back(index);
    }

    for (const StableFlatBlockAllocation &allocation :
         planStableFlatBlocks(resident, gpuByteBudget, pointBudget)) {
        const std::size_t original = originalIndices[allocation.candidateIndex];
        result.selected.push_back({
            .candidateIndex = original,
            .pointCount = allocation.pointCount,
        });
        result.uploads.push_back(original);
        result.protectedBlocks.push_back(original);
        if (allocation.pointCount > 0) {
            coverage[candidates[original].layerId] = true;
        }
    }
    result.coverage.reserve(coverage.size());
    for (const auto &[layerId, covered] : coverage) {
        result.coverage.push_back({.layerId = layerId, .covered = covered});
    }
    std::ranges::sort(result.coverage, {}, &FlatFrameLayerCoverage::layerId);
    return result;
}

std::vector<LayerPointBudgetAllocation> planLayerPointBudgets(
    const std::span<const LayerPointBudgetCandidate> candidates,
    const std::uint64_t pointBudget)
{
    if (pointBudget == 0 || candidates.empty()) {
        return {};
    }

    std::vector<std::uint64_t> allocations(candidates.size(), 0);
    std::uint64_t remaining = pointBudget;
    const auto stableOrder = [&candidates](const std::size_t left,
                                           const std::size_t right) {
        if (candidates[left].layerId != candidates[right].layerId) {
            return candidates[left].layerId < candidates[right].layerId;
        }
        return left < right;
    };

    // Water-fill toward every source's preferred coarse representation. A
    // small total budget therefore reduces all roots together instead of
    // completely dropping whichever source happens to come last.
    while (remaining > 0) {
        std::vector<std::size_t> active;
        active.reserve(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const std::uint64_t target = candidates[index].coveragePoints;
            if (allocations[index] < target) {
                active.push_back(index);
            }
        }
        if (active.empty()) {
            break;
        }
        std::ranges::sort(active, stableOrder);
        const std::uint64_t common = remaining / active.size();
        const std::uint64_t extra = remaining % active.size();
        std::uint64_t distributed = 0;
        for (std::size_t rank = 0; rank < active.size(); ++rank) {
            const std::size_t index = active[rank];
            const std::uint64_t target = candidates[index].coveragePoints;
            const std::uint64_t share = common + (rank < extra ? 1U : 0U);
            const std::uint64_t addition =
                std::min(target - allocations[index], share);
            allocations[index] += addition;
            distributed += addition;
        }
        if (distributed == 0) {
            break;
        }
        remaining -= distributed;
    }

    // Spend remaining detail by projected screen contribution. Capped sources
    // leave their unused share for another iteration.
    while (remaining > 0) {
        std::vector<std::size_t> active;
        active.reserve(candidates.size());
        long double totalWeight = 0.0L;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const std::uint64_t target =
                std::max(candidates[index].coveragePoints,
                         candidates[index].desiredPoints);
            if (allocations[index] < target) {
                active.push_back(index);
                totalWeight += usableWeight(candidates[index]);
            }
        }
        if (active.empty() || totalWeight <= 0.0L) {
            break;
        }

        struct Share {
            std::size_t index = 0;
            long double fractional = 0.0L;
            double weight = 0.0;
        };
        std::vector<Share> shares;
        shares.reserve(active.size());
        std::uint64_t distributed = 0;
        for (const std::size_t index : active) {
            const long double exact =
                static_cast<long double>(remaining) *
                static_cast<long double>(usableWeight(candidates[index])) /
                totalWeight;
            const std::uint64_t target =
                std::max(candidates[index].coveragePoints,
                         candidates[index].desiredPoints);
            const std::uint64_t deficit = target - allocations[index];
            const std::uint64_t whole = std::min<std::uint64_t>(
                deficit, static_cast<std::uint64_t>(std::floor(exact)));
            allocations[index] = saturatingAdd(allocations[index], whole);
            distributed += whole;
            shares.push_back({
                .index = index,
                .fractional = exact - std::floor(exact),
                .weight = usableWeight(candidates[index]),
            });
        }
        remaining -= std::min(remaining, distributed);
        if (remaining == 0) {
            break;
        }

        std::ranges::sort(shares,
                          [&candidates](const Share &left, const Share &right) {
                              if (left.fractional != right.fractional) {
                                  return left.fractional > right.fractional;
                              }
                              if (left.weight != right.weight) {
                                  return left.weight > right.weight;
                              }
                              if (candidates[left.index].layerId !=
                                  candidates[right.index].layerId) {
                                  return candidates[left.index].layerId <
                                         candidates[right.index].layerId;
                              }
                              return left.index < right.index;
                          });
        std::uint64_t extras = 0;
        for (const Share &share : shares) {
            if (remaining == 0) {
                break;
            }
            const std::uint64_t target =
                std::max(candidates[share.index].coveragePoints,
                         candidates[share.index].desiredPoints);
            if (allocations[share.index] >= target) {
                continue;
            }
            ++allocations[share.index];
            --remaining;
            ++extras;
        }
        if (distributed == 0 && extras == 0) {
            break;
        }
    }

    std::vector<LayerPointBudgetAllocation> result;
    result.reserve(candidates.size());
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        if (allocations[index] > 0) {
            result.push_back({
                .candidateIndex = index,
                .pointBudget = allocations[index],
            });
        }
    }
    std::ranges::sort(result,
                      [&stableOrder](const LayerPointBudgetAllocation &left,
                                     const LayerPointBudgetAllocation &right) {
                          return stableOrder(left.candidateIndex,
                                             right.candidateIndex);
                      });
    return result;
}

double projectedBoundsContribution(const Bounds3d &bounds,
                                   const Vec3d eye) noexcept
{
    if (!bounds.valid() || !isFinite(eye)) {
        return 1.0;
    }
    const auto centerArray = bounds.center();
    const Vec3d center{centerArray[0], centerArray[1], centerArray[2]};
    const Vec3d diagonal{
        bounds.maximum[0] - bounds.minimum[0],
        bounds.maximum[1] - bounds.minimum[1],
        bounds.maximum[2] - bounds.minimum[2],
    };
    const double radius = std::max(length(diagonal) * 0.5, 1e-9);
    const double distance = std::max(length(center - eye) - radius, radius);
    const double ratio = radius / distance;
    return std::clamp(ratio * ratio, 1e-12, 1.0);
}

} // namespace pci
