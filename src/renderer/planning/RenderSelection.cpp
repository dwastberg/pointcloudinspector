#include "renderer/planning/RenderSelection.h"

#include "foundation/CheckedArithmetic.h"
#include "renderer/planning/PointSizePolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace pci {
namespace {

double projectedErrorPixels(const PointCloudNode &node,
                            const RenderSelectionParameters &parameters)
{
    FrameCamera camera;
    camera.eye = parameters.eye;
    camera.outputHeight = std::max(parameters.viewportHeight, 1);
    camera.verticalFovDegrees = parameters.verticalFieldOfViewDegrees;
    camera.orthographicScale = parameters.orthographicScale;
    camera.orthographic = parameters.orthographic;
    return projectedPointSpacingPixels(
        node.bounds, node.geometricError, camera);
}

void appendUnique(
    std::vector<PointCloudNodeId> &nodes,
    std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> &seen,
    const PointCloudNodeId id)
{
    if (seen.insert(id).second) {
        nodes.push_back(id);
    }
}

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

std::uint64_t stablePriority(const StableFlatBlockCandidate &candidate) noexcept
{
    return splitMix64(candidate.blockId ^ splitMix64(candidate.groupId));
}

std::uint64_t fullDetailUsableBudget(const std::uint64_t byteBudget) noexcept
{
    const std::uint64_t reserve =
        byteBudget / 8U + (byteBudget % 8U != 0 ? 1U : 0U);
    return reserve > byteBudget ? 0 : byteBudget - reserve;
}

} // namespace

bool fullDetailResidencyFits(
    const std::span<const FullDetailResidencyCandidate> candidates,
    const std::uint64_t documentDecodedByteBudget,
    const std::uint64_t gpuByteBudget) noexcept
{
    if (candidates.empty() || documentDecodedByteBudget == 0 ||
        gpuByteBudget == 0) {
        return false;
    }
    std::uint64_t decodedBytes = 0;
    std::uint64_t gpuBytes = 0;
    for (const FullDetailResidencyCandidate &candidate : candidates) {
        if (candidate.sourcePointCount == 0 ||
            candidate.detailPointCount != candidate.sourcePointCount ||
            candidate.decodedBytes == 0 || candidate.gpuBytes == 0 ||
            candidate.decodedBytes >
                fullDetailUsableBudget(candidate.decodedByteBudget)) {
            return false;
        }
        decodedBytes = saturatingAdd(decodedBytes, candidate.decodedBytes);
        gpuBytes = saturatingAdd(gpuBytes, candidate.gpuBytes);
    }
    return decodedBytes <= fullDetailUsableBudget(documentDecodedByteBudget) &&
           gpuBytes <= fullDetailUsableBudget(gpuByteBudget);
}

bool ScreenPickVolume::valid() const noexcept
{
    return isFinite(origin) && isFinite(direction) &&
           std::isfinite(tangentHalfAngle) && tangentHalfAngle >= 0.0 &&
           std::isfinite(nearDistance) && nearDistance >= 0.0 &&
           std::isfinite(farDistance) && farDistance > nearDistance &&
           length(direction) > 0.0;
}

ScreenPickVolume makeScreenPickVolume(const Vec3d origin,
                                      const Vec3d forward,
                                      const Vec3d right,
                                      const Vec3d up,
                                      const double verticalFieldOfViewDegrees,
                                      const int viewportWidth,
                                      const int viewportHeight,
                                      const double pixelX,
                                      const double pixelY,
                                      const double radiusPixels,
                                      const double nearDistance,
                                      const double farDistance) noexcept
{
    if (!isFinite(origin) || !isFinite(forward) || !isFinite(right) ||
        !isFinite(up) || !std::isfinite(verticalFieldOfViewDegrees) ||
        verticalFieldOfViewDegrees <= 0.0 ||
        verticalFieldOfViewDegrees >= 180.0 || viewportWidth <= 0 ||
        viewportHeight <= 0 || !std::isfinite(pixelX) ||
        !std::isfinite(pixelY) || !std::isfinite(radiusPixels) ||
        radiusPixels < 0.0 || !std::isfinite(nearDistance) ||
        !std::isfinite(farDistance) || nearDistance < 0.0 ||
        farDistance <= nearDistance) {
        return {};
    }

    const double tangentVertical =
        std::tan(verticalFieldOfViewDegrees * std::numbers::pi / 360.0);
    const double aspect = static_cast<double>(viewportWidth) /
                          static_cast<double>(viewportHeight);
    const double ndcX =
        2.0 * (pixelX + 0.5) / static_cast<double>(viewportWidth) - 1.0;
    const double ndcY =
        1.0 - 2.0 * (pixelY + 0.5) / static_cast<double>(viewportHeight);
    const Vec3d direction =
        normalized(normalized(forward) +
                   normalized(right) * (ndcX * tangentVertical * aspect) +
                   normalized(up) * (ndcY * tangentVertical));
    // The corner of a square pick footprint is farther from its centre than
    // an edge. Include that corner so near-edge rasterized points are never
    // discarded by the CPU broad phase.
    const double tangentHalfAngle = radiusPixels * std::numbers::sqrt2 * 2.0 *
                                    tangentVertical /
                                    static_cast<double>(viewportHeight);
    return {
        .origin = origin,
        .direction = direction,
        .tangentHalfAngle = tangentHalfAngle,
        .nearDistance = nearDistance,
        .farDistance = farDistance,
    };
}

double screenPickDistance(const ScreenPickVolume &volume,
                          const Bounds3d &bounds) noexcept
{
    if (!volume.valid() || !bounds.valid()) {
        return std::numeric_limits<double>::infinity();
    }
    const auto center = bounds.center();
    const Vec3d halfExtents{
        (bounds.maximum[0] - bounds.minimum[0]) * 0.5,
        (bounds.maximum[1] - bounds.minimum[1]) * 0.5,
        (bounds.maximum[2] - bounds.minimum[2]) * 0.5,
    };
    return dot(Vec3d{center[0], center[1], center[2]} - volume.origin,
               normalized(volume.direction)) -
           length(halfExtents);
}

bool intersectsScreenPickVolume(const ScreenPickVolume &volume,
                                const Bounds3d &bounds,
                                const double expansion) noexcept
{
    if (!volume.valid() || !bounds.valid() || !std::isfinite(expansion) ||
        expansion < 0.0) {
        return false;
    }
    const auto centerArray = bounds.center();
    const Vec3d center{centerArray[0], centerArray[1], centerArray[2]};
    const Vec3d halfExtents{
        (bounds.maximum[0] - bounds.minimum[0]) * 0.5,
        (bounds.maximum[1] - bounds.minimum[1]) * 0.5,
        (bounds.maximum[2] - bounds.minimum[2]) * 0.5,
    };
    const double radius = length(halfExtents) + expansion;
    const Vec3d fromOrigin = center - volume.origin;
    const Vec3d direction = normalized(volume.direction);
    const double along = dot(fromOrigin, direction);
    if (along + radius < volume.nearDistance ||
        along - radius > volume.farDistance) {
        return false;
    }
    const double radialSquared =
        std::max(0.0, dot(fromOrigin, fromOrigin) - along * along);
    const double coneDistance =
        std::clamp(along, volume.nearDistance, volume.farDistance);
    const double allowedRadius =
        radius + (coneDistance + radius) * volume.tangentHalfAngle;
    return radialSquared <= allowedRadius * allowedRadius;
}

std::vector<StableFlatBlockAllocation>
planStableFlatBlocks(const std::span<const StableFlatBlockCandidate> candidates,
                     const std::uint64_t gpuByteBudget,
                     const std::uint64_t pointBudget)
{
    if (candidates.empty() || gpuByteBudget == 0 || pointBudget == 0) {
        return {};
    }

    std::vector<std::size_t> ordered;
    ordered.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].pointCount > 0 &&
            candidates[index].gpuBytes > 0) {
            ordered.push_back(index);
        }
    }
    std::ranges::sort(
        ordered,
        [&candidates](const std::size_t left, const std::size_t right) {
            const StableFlatBlockCandidate &leftCandidate = candidates[left];
            const StableFlatBlockCandidate &rightCandidate = candidates[right];
            const std::uint64_t leftPriority = stablePriority(leftCandidate);
            const std::uint64_t rightPriority = stablePriority(rightCandidate);
            if (leftPriority != rightPriority) {
                return leftPriority < rightPriority;
            }
            if (leftCandidate.groupId != rightCandidate.groupId) {
                return leftCandidate.groupId < rightCandidate.groupId;
            }
            if (leftCandidate.blockId != rightCandidate.blockId) {
                return leftCandidate.blockId < rightCandidate.blockId;
            }
            return left < right;
        });

    // Reserve one complete upload block per source before admitting detail
    // blocks. Within a source, prefer the smallest block and use the stable
    // spatial priority as its tie-breaker. This maximizes the chance that all
    // sources receive coverage when GPU residency is tight.
    std::unordered_map<std::uint64_t, std::size_t> coverageByGroup;
    for (const std::size_t index : ordered) {
        const StableFlatBlockCandidate &candidate = candidates[index];
        const auto found = coverageByGroup.find(candidate.groupId);
        if (found == coverageByGroup.end() ||
            candidate.gpuBytes < candidates[found->second].gpuBytes ||
            (candidate.gpuBytes == candidates[found->second].gpuBytes &&
             stablePriority(candidate) <
                 stablePriority(candidates[found->second]))) {
            coverageByGroup[candidate.groupId] = index;
        }
    }
    std::vector<std::size_t> coverage;
    coverage.reserve(coverageByGroup.size());
    for (const auto &[group, index] : coverageByGroup) {
        static_cast<void>(group);
        coverage.push_back(index);
    }
    std::ranges::sort(
        coverage,
        [&candidates](const std::size_t left, const std::size_t right) {
            if (candidates[left].groupId != candidates[right].groupId) {
                return candidates[left].groupId < candidates[right].groupId;
            }
            return left < right;
        });

    std::vector<std::size_t> resident;
    resident.reserve(ordered.size());
    std::unordered_set<std::size_t> residentSet;
    std::uint64_t remainingBytes = gpuByteBudget;
    std::uint64_t residentPoints = 0;
    for (const std::size_t index : coverage) {
        const StableFlatBlockCandidate &candidate = candidates[index];
        if (candidate.gpuBytes > remainingBytes) {
            continue;
        }
        resident.push_back(index);
        residentSet.insert(index);
        remainingBytes -= candidate.gpuBytes;
        residentPoints = saturatingAdd(residentPoints, candidate.pointCount);
    }
    for (const std::size_t index : ordered) {
        if (residentSet.contains(index)) {
            continue;
        }
        const StableFlatBlockCandidate &candidate = candidates[index];
        if (candidate.gpuBytes > remainingBytes) {
            continue;
        }
        resident.push_back(index);
        residentSet.insert(index);
        remainingBytes -= candidate.gpuBytes;
        residentPoints = saturatingAdd(residentPoints, candidate.pointCount);
    }
    if (resident.empty() || residentPoints == 0) {
        return {};
    }

    // Residency priority may be hashed for spatial stability, but draw order
    // remains the stable document/block order. Point picking assigns global
    // IDs in draw order and must not change merely because residency policy
    // was introduced.
    std::ranges::sort(
        resident,
        [&candidates](const std::size_t left, const std::size_t right) {
            const StableFlatBlockCandidate &leftCandidate = candidates[left];
            const StableFlatBlockCandidate &rightCandidate = candidates[right];
            if (leftCandidate.groupId != rightCandidate.groupId) {
                return leftCandidate.groupId < rightCandidate.groupId;
            }
            if (leftCandidate.blockId != rightCandidate.blockId) {
                return leftCandidate.blockId < rightCandidate.blockId;
            }
            return left < right;
        });

    const std::uint64_t effectiveBudget = std::min(pointBudget, residentPoints);
    std::vector<LayerPointBudgetCandidate> groupCandidates;
    std::unordered_map<std::uint64_t, std::size_t> groupCandidateIndices;
    for (const std::size_t index : resident) {
        const StableFlatBlockCandidate &candidate = candidates[index];
        auto [found, inserted] = groupCandidateIndices.emplace(
            candidate.groupId, groupCandidates.size());
        if (inserted) {
            groupCandidates.push_back({
                .layerId = candidate.groupId,
                .coveragePoints = candidate.pointCount,
                .desiredPoints = candidate.pointCount,
            });
        } else {
            groupCandidates[found->second].desiredPoints =
                saturatingAdd(groupCandidates[found->second].desiredPoints,
                              candidate.pointCount);
        }
    }
    std::unordered_map<std::uint64_t, std::uint64_t> groupBudgets;
    for (const LayerPointBudgetAllocation &allocation :
         planLayerPointBudgets(groupCandidates, effectiveBudget)) {
        groupBudgets[groupCandidates[allocation.candidateIndex].layerId] =
            allocation.pointBudget;
    }

    std::vector<StableFlatBlockAllocation> result;
    result.reserve(resident.size());
    std::unordered_map<std::uint64_t, std::uint64_t> groupCumulativePoints;
    std::unordered_map<std::uint64_t, std::uint64_t> groupAllocatedPoints;
    std::unordered_map<std::uint64_t, std::uint64_t> groupResidentPoints;
    for (const std::size_t index : resident) {
        groupResidentPoints[candidates[index].groupId] =
            saturatingAdd(groupResidentPoints[candidates[index].groupId],
                          candidates[index].pointCount);
    }
    for (const std::size_t index : resident) {
        const StableFlatBlockCandidate &candidate = candidates[index];
        std::uint64_t &cumulativePoints =
            groupCumulativePoints[candidate.groupId];
        std::uint64_t &allocatedPoints =
            groupAllocatedPoints[candidate.groupId];
        cumulativePoints =
            saturatingAdd(cumulativePoints, candidate.pointCount);
        const std::uint64_t groupBudget = groupBudgets[candidate.groupId];
        const std::uint64_t groupPoints =
            groupResidentPoints[candidate.groupId];
        std::uint64_t cumulativeAllocation = groupBudget;
        if (cumulativePoints < groupPoints) {
            const long double scaled =
                static_cast<long double>(cumulativePoints) *
                static_cast<long double>(groupBudget) /
                static_cast<long double>(groupPoints);
            cumulativeAllocation = std::min<std::uint64_t>(
                groupBudget, static_cast<std::uint64_t>(std::floor(scaled)));
        }
        cumulativeAllocation = std::max(cumulativeAllocation, allocatedPoints);
        const std::uint64_t allocation = std::min(
            candidate.pointCount, cumulativeAllocation - allocatedPoints);
        if (allocation > 0) {
            result.push_back({
                .candidateIndex = index,
                .pointCount = allocation,
            });
        }
        allocatedPoints += allocation;
    }
    return result;
}

RenderSelectionResult
RenderSelection::select(const std::span<const PointCloudNodeId> roots,
                        const NodeLookup &lookup,
                        const VisibilityTest &visible,
                        const RenderSelectionParameters &parameters)
{
    RenderSelectionResult result;
    if (!lookup || !visible || parameters.pointBudget == 0) {
        refinedLastFrame_.clear();
        return result;
    }

    std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> requested;
    std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> nextRefined;
    std::uint64_t remaining = parameters.pointBudget;
    std::uint64_t requestRemaining = parameters.requestPointBudget == 0
                                         ? parameters.pointBudget
                                         : parameters.requestPointBudget;
    const auto requestNode = [&result, &requested, &requestRemaining](
                                 const RenderSelectionNodeState &state) {
        if (requested.contains(state.node.id)) {
            return true;
        }
        const std::uint64_t cost =
            state.resident ? 0
                           : std::max(state.residentPointCount,
                                      state.node.estimatedPointCount);
        if (cost > requestRemaining && !requested.empty()) {
            return false;
        }
        appendUnique(result.requestedNodes, requested, state.node.id);
        requestRemaining =
            cost >= requestRemaining ? 0 : requestRemaining - cost;
        return true;
    };

    const auto selectVisibleNode =
        [&](const auto &self,
            const RenderSelectionNodeState &state,
            const std::uint64_t allowance) -> std::uint64_t {
        if (!state.resident) {
            static_cast<void>(requestNode(state));
            return 0;
        }
        if (state.residentPointCount > allowance) {
            return 0;
        }

        const double errorPixels = projectedErrorPixels(state.node, parameters);
        const bool wasRefined = refinedLastFrame_.contains(state.node.id);
        const bool wantsRefinement =
            !state.node.leaf && !state.node.detailLimited &&
            errorPixels > (wasRefined ? parameters.coarsenPixelError
                                      : parameters.refinePixelError);

        if (wantsRefinement) {
            nextRefined.insert(state.node.id);
            std::vector<RenderSelectionNodeState> visibleChildren;
            for (const PointCloudNodeId child : childNodeIds(state.node.id)) {
                RenderSelectionNodeState childState = lookup(child);
                if (!childState.node.bounds.valid() ||
                    !visible(childState.node.bounds)) {
                    continue;
                }
                visibleChildren.push_back(childState);
            }
            std::ranges::stable_sort(
                visibleChildren,
                [&parameters](const RenderSelectionNodeState &left,
                              const RenderSelectionNodeState &right) {
                    return projectedErrorPixels(left.node, parameters) >
                           projectedErrorPixels(right.node, parameters);
                });
            for (const RenderSelectionNodeState &child : visibleChildren) {
                static_cast<void>(requestNode(child));
            }

            const bool childrenReady = std::ranges::all_of(
                visibleChildren, [](const RenderSelectionNodeState &child) {
                    return child.resident;
                });
            const std::uint64_t childPoints = std::accumulate(
                visibleChildren.begin(),
                visibleChildren.end(),
                std::uint64_t{0},
                [](const std::uint64_t total,
                   const RenderSelectionNodeState &child) {
                    return saturatingAdd(total, child.residentPointCount);
                });
            if (!visibleChildren.empty() && childrenReady &&
                childPoints <= allowance) {
                std::array<std::uint64_t, 9> suffixPoints{};
                for (std::size_t index = visibleChildren.size(); index > 0;
                     --index) {
                    suffixPoints[index - 1U] = saturatingAdd(
                        suffixPoints[index],
                        visibleChildren[index - 1U].residentPointCount);
                }
                std::uint64_t childRemaining = allowance;
                for (std::size_t index = 0; index < visibleChildren.size();
                     ++index) {
                    const std::uint64_t reservedForLater =
                        suffixPoints[index + 1U];
                    const std::uint64_t childAllowance =
                        childRemaining - reservedForLater;
                    const std::uint64_t selected =
                        self(self, visibleChildren[index], childAllowance);
                    childRemaining -= selected;
                }
                return allowance - childRemaining;
            }
        }

        result.drawNodes.push_back(state.node.id);
        result.selectedPoints =
            saturatingAdd(result.selectedPoints, state.residentPointCount);
        return state.residentPointCount;
    };

    for (const PointCloudNodeId root : roots) {
        if (remaining == 0) {
            break;
        }
        const RenderSelectionNodeState state = lookup(root);
        if (!state.node.bounds.valid() || !visible(state.node.bounds)) {
            continue;
        }
        const std::uint64_t selected =
            selectVisibleNode(selectVisibleNode, state, remaining);
        remaining -= selected;
    }
    refinedLastFrame_ = std::move(nextRefined);
    return result;
}

void RenderSelection::reset()
{
    refinedLastFrame_.clear();
}

} // namespace pci
