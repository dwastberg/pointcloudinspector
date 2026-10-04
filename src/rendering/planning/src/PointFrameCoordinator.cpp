#include <pci/rendering/planning/PointFrameCoordinator.h>

#include <pci/foundation/Hash.h>
#include <pci/pointcloud/GpuPoint.h>
#include <pci/pointcloud/PointAttributes.h>
#include <pci/rendering/planning/FramePlanner.h>

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace pci {
namespace {

constexpr std::uint64_t hierarchyRequestPointBudget = 2'000'000;

std::uint64_t usableTransitionBudget(const std::uint64_t byteBudget) noexcept
{
    const std::uint64_t reserve =
        byteBudget / 8U + (byteBudget % 8U != 0 ? 1U : 0U);
    return reserve > byteBudget ? 0 : byteBudget - reserve;
}

void saturatingAdd(std::uint64_t &value, const std::uint64_t increment) noexcept
{
    value = increment > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + increment;
}

void finalizeCoverage(PointFramePlan &plan, const std::uint64_t visibleLayers)
{
    std::unordered_set<PointCloudLayerId> covered;
    for (const PointFrameSelectedBlock &block : plan.blocks) {
        if (block.pointCount > 0) {
            covered.insert(block.layerId);
        }
    }
    plan.visibleLayerCount = visibleLayers;
    plan.coveredLayerCount = covered.size();
}

PointFrameRuntimeTarget runtimeTarget(const PointFrameLayer &layer)
{
    return {
        .layerId = layer.layerId,
        .sourceId = layer.sourceId,
        .bindingGeneration = layer.bindingGeneration,
        .contentRevision =
            layer.residency ? layer.residency->contentRevision() : 0,
    };
}

} // namespace

std::size_t
PointFrameBlockKeyHash::operator()(const PointFrameBlockKey &key) const noexcept
{
    std::size_t result = std::hash<PointCloudLayerId>{}(key.layerId);
    result = hashCombine(result, PointCloudNodeIdHash{}(key.nodeId));
    result =
        hashCombine(result, std::hash<std::uint32_t>{}(key.nodeBlockIndex));
    return hashCombine(result, std::hash<std::uint64_t>{}(key.sceneBlockId));
}

FlatFramePlan PointFrameCoordinator::planFlatCandidates(
    const std::span<const FlatFrameBlockCandidate> candidates,
    const std::uint64_t gpuByteBudget,
    const std::uint64_t pointBudget)
{
    return planFlatFrame(candidates, gpuByteBudget, pointBudget);
}

void PointFrameCoordinator::clear()
{
    visibilityIndex_.clear();
    hierarchySelections_.clear();
    cachedFlatPlanKey_.reset();
    cachedFlatPlan_.reset();
    flatBudgetSettlementKey_.reset();
}

PointBudgetUpdate PointFrameCoordinator::pointBudgetUpdate(
    const std::vector<PointFrameLayer> &layers,
    const std::uint64_t decodedByteBudget,
    const std::uint64_t gpuByteBudget,
    const std::uint64_t currentPointBudget)
{
    std::uint64_t capacity = 0;
    for (const PointFrameLayer &layer : layers) {
        saturatingAdd(capacity,
                      layer.snapshot->hierarchical
                          ? layer.snapshot->sourcePointCount
                          : layer.snapshot->retainedFlatPointCount);
    }
    const bool allFlat =
        std::ranges::all_of(layers, [](const PointFrameLayer &layer) {
            return !layer.snapshot->hierarchical;
        });
    const bool allFlatComplete =
        !layers.empty() && allFlat &&
        std::ranges::all_of(layers, [](const PointFrameLayer &layer) {
            return layer.snapshot->loadingComplete;
        });
    const bool allHierarchicalComplete =
        !layers.empty() &&
        std::ranges::all_of(layers, [](const PointFrameLayer &layer) {
            return layer.snapshot->hierarchical &&
                   layer.snapshot->loadingComplete;
        });
    if (allFlat) {
        capacity = std::min(capacity, gpuByteBudget / sizeof(GpuPoint));
    }
    PointBudgetUpdate result{
        .total = std::max<std::uint64_t>(capacity, 1),
        .current = std::nullopt,
    };
    const std::uint64_t decodedBytesPerPoint =
        sizeof(GpuPoint) + sizeof(PointAttributes);
    const std::uint64_t stableDecodedPointCapacity =
        usableTransitionBudget(decodedByteBudget) / decodedBytesPerPoint;
    const std::uint64_t stableGpuPointCapacity =
        usableTransitionBudget(gpuByteBudget) / sizeof(GpuPoint);
    const bool stableHierarchyFits = allHierarchicalComplete &&
                                     capacity <= stableDecodedPointCapacity &&
                                     capacity <= stableGpuPointCapacity;
    if (stableHierarchyFits) {
        // Keep small, completed paged sources visually stable without
        // resurrecting whole-hierarchy warming: selection remains
        // screen-space and requests only visible nodes.
        flatBudgetSettlementKey_.reset();
        result.current = result.total;
    } else if (allFlatComplete) {
        FlatBudgetSettlementKey key{
            .gpuByteBudget = gpuByteBudget,
            .retainedLayerPoints = {},
        };
        key.retainedLayerPoints.reserve(layers.size());
        for (const PointFrameLayer &layer : layers) {
            key.retainedLayerPoints.emplace_back(
                layer.layerId, layer.snapshot->retainedFlatPointCount);
        }
        if (flatBudgetSettlementKey_ != key) {
            result.current = result.total;
            flatBudgetSettlementKey_ = std::move(key);
        }
    } else {
        flatBudgetSettlementKey_.reset();
        // A viewport is commonly attached while the document is still empty.
        // Its one-point controller must recover when an asynchronously loaded
        // hierarchy later publishes its real source count. Full-detail mode
        // used to hide this by replacing the budget; the ordinary LOD path
        // needs the same interactive bootstrap as a streaming flat scene.
        result.current =
            std::max(currentPointBudget,
                     std::min<std::uint64_t>(1'000'000, result.total));
    }
    return result;
}

PointFrameResult PointFrameCoordinator::plan(PointFrameInput input)
{
    const bool allFlat =
        std::ranges::all_of(input.layers, [](const PointFrameLayer &layer) {
            return !layer.snapshot->hierarchical;
        });
    if (!allFlat) {
        cachedFlatPlanKey_.reset();
        cachedFlatPlan_.reset();
        return {
            .plan = std::make_shared<const PointFramePlan>(buildPlan(input)),
            .execution = nextExecution(input),
        };
    }

    FlatFramePlanKey key = flatPlanKey(input);
    if (cachedFlatPlanKey_ == key && cachedFlatPlan_) {
        return {.plan = cachedFlatPlan_,
                .execution = nextExecution(input),
                .reused = true};
    }
    cachedFlatPlan_ = std::make_shared<const PointFramePlan>(buildPlan(input));
    cachedFlatPlanKey_ = std::move(key);
    return {.plan = cachedFlatPlan_, .execution = nextExecution(input)};
}

PointFrameExecutionIdentity
PointFrameCoordinator::nextExecution(const PointFrameInput &input)
{
    if (executionSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "point frame execution serials are exhausted");
    }
    return {
        .sessionGeneration = input.sessionGeneration,
        .documentGeneration = input.documentGeneration,
        .documentRevision = input.documentRevision,
        .serial = ++executionSerial_,
    };
}

PointFramePlan PointFrameCoordinator::buildPlan(const PointFrameInput &input)
{
    const FrameCamera &frame = input.camera;
    const FrustumCuller &culler = frame.culler;
    const auto resident = [&input](const PointFrameBlockKey &key) {
        return input.resident(key);
    };
    PointFramePlan result;
    const auto lookupPayload = [&result](const PointFrameLayer &layer,
                                         const PointCloudNodeId id) {
        const PointCloudNodePayloadPtr payload = layer.residency->acquire(id);
        result.decodedLookupEffects.push_back({
            .target = runtimeTarget(layer),
            .nodeId = id,
            .resident = static_cast<bool>(payload),
        });
        return payload;
    };
    std::unordered_set<PointFrameBlockKey, PointFrameBlockKeyHash>
        scheduledUploads;
    const auto scheduleUpload =
        [&result, &scheduledUploads](const PointFrameBlockKey &key,
                                     const PointBlockPtr &block) {
            if (scheduledUploads.insert(key).second) {
                result.uploads.push_back({.key = key, .block = block});
            }
        };

    const bool allFlat =
        std::ranges::all_of(input.layers, [](const PointFrameLayer &layer) {
            return !layer.snapshot->hierarchical;
        });
    const std::uint64_t gpuPointCapacity =
        input.gpuByteBudget / sizeof(GpuPoint);
    const std::uint64_t transitionSafeCapacity =
        allFlat ? gpuPointCapacity
                : (gpuPointCapacity >= 9
                       ? gpuPointCapacity - (gpuPointCapacity + 8U) / 9U
                       : gpuPointCapacity);
    const std::uint64_t availablePoints =
        std::min(input.framePointBudget, transitionSafeCapacity);

    std::vector<SceneVisibilityLayer> visibilityLayers;
    visibilityLayers.reserve(input.layers.size());
    for (const PointFrameLayer &layer : input.layers) {
        visibilityLayers.push_back({
            .layerId = layer.layerId,
            .sourceBounds = layer.sourceBounds,
        });
    }
    visibilityIndex_.update(
        input.documentGeneration, input.documentRevision, visibilityLayers);
    const std::vector<PointCloudLayerId> intersecting =
        visibilityIndex_.visibleLayersIntersecting(
            [&culler](const Bounds3d &bounds) {
                return culler.intersects(bounds);
            });
    std::unordered_set<PointCloudLayerId> inFrustumLayers(intersecting.begin(),
                                                          intersecting.end());

    std::vector<LayerPointBudgetCandidate> layerCandidates;
    if (!allFlat) {
        layerCandidates.reserve(inFrustumLayers.size());
        for (const PointFrameLayer &layer : input.layers) {
            if (!inFrustumLayers.contains(layer.layerId)) {
                continue;
            }
            std::uint64_t coveragePoints = 0;
            std::uint64_t desiredPoints = 0;
            if (layer.snapshot->hierarchical) {
                if (!layer.residency ||
                    layer.residency->sourceId() != layer.sourceId) {
                    continue;
                }
                const PointCloudNodePayloadPtr root =
                    lookupPayload(layer, rootPointCloudNode);
                coveragePoints = root
                                     ? pointCloudNodePayloadPoints(*root)
                                     : layer.residency->node(rootPointCloudNode)
                                           .estimatedPointCount;
                desiredPoints = layer.snapshot->sourcePointCount;
            } else {
                desiredPoints = layer.snapshot->retainedFlatPointCount;
                for (const PointDatasetRuntimeBlockSnapshot &entry :
                     layer.snapshot->flatBlocks) {
                    if (culler.intersects(entry.block->bounds)) {
                        const std::uint64_t blockPoints =
                            entry.block->points.size();
                        coveragePoints =
                            coveragePoints == 0
                                ? blockPoints
                                : std::min(coveragePoints, blockPoints);
                    }
                }
            }
            if (coveragePoints == 0 || desiredPoints == 0) {
                continue;
            }
            layerCandidates.push_back({
                .layerId = layer.layerId.value(),
                .coveragePoints = coveragePoints,
                .desiredPoints = desiredPoints,
                .projectedContribution = projectedBoundsContribution(
                    layer.snapshot->bounds, frame.eye),
            });
        }
    }
    std::unordered_map<PointCloudLayerId, std::uint64_t> layerBudgets;
    for (const LayerPointBudgetAllocation &allocation :
         planLayerPointBudgets(layerCandidates, availablePoints)) {
        layerBudgets[PointCloudLayerId{
            layerCandidates[allocation.candidateIndex].layerId}] =
            allocation.pointBudget;
    }

    struct VisibleBlock {
        const PointFrameLayer *layer = nullptr;
        std::uint64_t sceneBlockId = 0;
        PointBlockPtr block;
        double distanceSquared = 0.0;
    };
    std::vector<VisibleBlock> visible;
    for (const PointFrameLayer &layer : input.layers) {
        const PointResidencyViewPtr &residency = layer.residency;
        if (!inFrustumLayers.contains(layer.layerId)) {
            result.outOfFrustumLayerIds.insert(layer.layerId);
            if (layer.snapshot->hierarchical) {
                result.nodeRequests.push_back({
                    .target = runtimeTarget(layer),
                    .nodes = {},
                });
                result.trimRequests.push_back({
                    .target = runtimeTarget(layer),
                    .protectedNodes = {},
                });
                hierarchySelections_[layer.layerId].reset();
                ++result.culledBlocks;
            } else {
                result.culledBlocks += layer.snapshot->flatBlocks.size();
            }
            continue;
        }
        if (layer.snapshot->hierarchical) {
            if (!residency || residency->sourceId() != layer.sourceId) {
                result.layerErrors.push_back({
                    .layerId = layer.layerId,
                    .message = "point residency view is unavailable or stale",
                });
                result.nodeRequests.push_back({
                    .target = runtimeTarget(layer),
                    .nodes = {},
                });
                hierarchySelections_.erase(layer.layerId);
                continue;
            }
            std::uint64_t layerRemaining = layerBudgets[layer.layerId];
            if (layerRemaining == 0) {
                result.nodeRequests.push_back({
                    .target = runtimeTarget(layer),
                    .nodes = {},
                });
                continue;
            }
            result.trimRequests.push_back({
                .target = runtimeTarget(layer),
                .protectedNodes = {},
            });
            PointFrameTrimRequest &trimRequest = result.trimRequests.back();
            const std::string error(residency->error());
            if (!error.empty()) {
                result.layerErrors.push_back({
                    .layerId = layer.layerId,
                    .message = error,
                });
                result.nodeRequests.push_back({
                    .target = runtimeTarget(layer),
                    .nodes = {},
                });
                hierarchySelections_.erase(layer.layerId);
                continue;
            }
            RenderSelection &selector = hierarchySelections_[layer.layerId];
            const std::array roots{rootPointCloudNode};
            const auto blockKey = [&layer](const PointCloudNodeId nodeId,
                                           const std::size_t index) {
                return PointFrameBlockKey{
                    .layerId = layer.layerId,
                    .nodeId = nodeId,
                    .nodeBlockIndex = static_cast<std::uint32_t>(index),
                };
            };
            const RenderSelectionResult selection = selector.select(
                roots,
                [&layer, &residency, &blockKey, &resident, &lookupPayload](
                    const PointCloudNodeId id) {
                    const PointCloudNodePayloadPtr payload =
                        lookupPayload(layer, id);
                    bool drawable = static_cast<bool>(payload);
                    if (payload) {
                        for (std::size_t index = 0;
                             index < payload->blocks.size();
                             ++index) {
                            if (!resident(blockKey(id, index))) {
                                drawable = false;
                                break;
                            }
                        }
                    }
                    return RenderSelectionNodeState{
                        .node = residency->node(id),
                        .resident = drawable,
                        .residentPointCount =
                            payload ? pointCloudNodePayloadPoints(*payload) : 0,
                    };
                },
                [&culler](const Bounds3d &bounds) {
                    return culler.intersects(bounds);
                },
                {.eye = frame.eye,
                 .verticalFieldOfViewDegrees = frame.verticalFovDegrees,
                 .viewportHeight = frame.outputHeight,
                 .orthographicScale = frame.orthographicScale,
                 .orthographic = frame.orthographic,
                 .pointBudget = layerRemaining,
                 .requestPointBudget = hierarchyRequestPointBudget});
            if (selection.drawNodes.size() == 1 &&
                selection.drawNodes.front() == rootPointCloudNode) {
                ++result.rootOnlyLayerCount;
            }
            result.nodeRequests.push_back({
                .target = runtimeTarget(layer),
                .nodes = selection.requestedNodes,
            });
            trimRequest.protectedNodes = selection.drawNodes;
            for (const PointCloudNodeId nodeId : selection.requestedNodes) {
                if (std::ranges::find(trimRequest.protectedNodes, nodeId) ==
                    trimRequest.protectedNodes.end()) {
                    trimRequest.protectedNodes.push_back(nodeId);
                }
            }

            std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash>
                leasedNodes;
            const auto scheduleNode = [&result,
                                       &leasedNodes,
                                       &layer,
                                       &blockKey,
                                       &scheduleUpload,
                                       &lookupPayload](
                                          const PointCloudNodeId nodeId) {
                PointCloudNodePayloadPtr payload = lookupPayload(layer, nodeId);
                if (!payload) {
                    return;
                }
                if (leasedNodes.insert(nodeId).second) {
                    result.decodedLeases.push_back(payload);
                }
                for (std::size_t index = 0; index < payload->blocks.size();
                     ++index) {
                    scheduleUpload(blockKey(nodeId, index),
                                   payload->blocks[index]);
                }
            };
            for (const PointCloudNodeId nodeId : selection.requestedNodes) {
                scheduleNode(nodeId);
            }
            for (const PointCloudNodeId nodeId : selection.drawNodes) {
                PointCloudNodePayloadPtr payload = lookupPayload(layer, nodeId);
                if (!payload) {
                    continue;
                }
                const PointCloudNode node = residency->node(nodeId);
                scheduleNode(nodeId);
                for (std::size_t index = 0;
                     index < payload->blocks.size() && layerRemaining > 0;
                     ++index) {
                    const PointBlockPtr &block = payload->blocks[index];
                    const auto pointCount =
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(
                            block->points.size(), layerRemaining));
                    const PointFrameBlockKey key = blockKey(nodeId, index);
                    result.blocks.push_back({
                        .layerId = layer.layerId,
                        .colorMode = layer.colorMode,
                        .classificationFilter = layer.classificationFilter,
                        .colorRange = layer.colorRange,
                        .key = key,
                        .block = block,
                        .pointCount = pointCount,
                        .pointSpacing = node.geometricError,
                        .pointCoverageFactor = node.leaf ? 1.0 : 2.0,
                    });
                    saturatingAdd(result.selectedPoints, pointCount);
                    ++result.visibleBlocks;
                    result.protectedGpuBlocks.push_back(key);
                    layerRemaining -= pointCount;
                }
            }
            continue;
        }

        for (const PointDatasetRuntimeBlockSnapshot &entry :
             layer.snapshot->flatBlocks) {
            if (!culler.intersects(entry.block->bounds)) {
                ++result.culledBlocks;
                continue;
            }
            double distanceSquared = 0.0;
            if (!allFlat) {
                const Vec3d difference = entry.center - frame.eye;
                distanceSquared = dot(difference, difference);
            }
            visible.push_back({.layer = &layer,
                               .sceneBlockId = entry.id,
                               .block = entry.block,
                               .distanceSquared = distanceSquared});
        }
    }

    if (allFlat) {
        std::vector<FlatFrameBlockCandidate> candidates;
        candidates.reserve(visible.size());
        for (const VisibleBlock &block : visible) {
            candidates.push_back({
                .layerId = block.layer->layerId.value(),
                .blockId = block.sceneBlockId,
                .pointCount = block.block->points.size(),
                .gpuBytes = block.block->points.size() * sizeof(GpuPoint),
            });
        }
        const FlatFramePlan flatPlan = planFlatCandidates(
            candidates, input.gpuByteBudget, input.framePointBudget);
        result.visibleBlocks = flatPlan.visibleBlockCount;
        for (const FlatFrameBlockSelection &allocation : flatPlan.selected) {
            const VisibleBlock &block = visible[allocation.candidateIndex];
            const PointFrameBlockKey key{
                .layerId = block.layer->layerId,
                .nodeId = {},
                .sceneBlockId = block.sceneBlockId,
            };
            result.blocks.push_back({
                .layerId = block.layer->layerId,
                .colorMode = block.layer->colorMode,
                .classificationFilter = block.layer->classificationFilter,
                .colorRange = block.layer->colorRange,
                .key = key,
                .block = block.block,
                .pointCount = static_cast<std::uint32_t>(allocation.pointCount),
            });
            saturatingAdd(result.selectedPoints, allocation.pointCount);
            scheduleUpload(key, block.block);
            result.protectedGpuBlocks.push_back(key);
        }
        result.visibleLayerCount = inFrustumLayers.size();
        result.coveredLayerCount =
            static_cast<std::uint64_t>(std::ranges::count(
                flatPlan.coverage, true, &FlatFrameLayerCoverage::covered));
        result.requiresContinuation = flatPlan.requiresContinuation;
        return result;
    }

    std::ranges::sort(visible,
                      [](const VisibleBlock &left, const VisibleBlock &right) {
                          return left.distanceSquared < right.distanceSquared;
                      });
    for (const VisibleBlock &block : visible) {
        std::uint64_t &layerRemaining = layerBudgets[block.layer->layerId];
        if (layerRemaining == 0) {
            continue;
        }
        const auto pointCount =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                block.block->points.size(), layerRemaining));
        const PointFrameBlockKey key{
            .layerId = block.layer->layerId,
            .nodeId = {},
            .sceneBlockId = block.sceneBlockId,
        };
        result.blocks.push_back({
            .layerId = block.layer->layerId,
            .colorMode = block.layer->colorMode,
            .classificationFilter = block.layer->classificationFilter,
            .colorRange = block.layer->colorRange,
            .key = key,
            .block = block.block,
            .pointCount = pointCount,
        });
        saturatingAdd(result.selectedPoints, pointCount);
        ++result.visibleBlocks;
        scheduleUpload(key, block.block);
        result.protectedGpuBlocks.push_back(key);
        layerRemaining -= pointCount;
    }
    finalizeCoverage(result, inFrustumLayers.size());
    return result;
}

PointFrameCoordinator::FlatFramePlanKey
PointFrameCoordinator::flatPlanKey(const PointFrameInput &input)
{
    FlatFramePlanKey key{
        .sessionGeneration = input.sessionGeneration,
        .documentGeneration = input.documentGeneration,
        .cameraRevision = input.cameraRevision,
        .documentRevision = input.documentRevision,
        .pointBudget = input.framePointBudget,
        .outputWidth = input.camera.outputWidth,
        .outputHeight = input.camera.outputHeight,
        .layers = {},
    };
    key.layers.reserve(input.layers.size());
    for (const PointFrameLayer &layer : input.layers) {
        key.layers.push_back({
            .layerId = layer.layerId,
            .sourceId = layer.sourceId,
            .bindingGeneration = layer.bindingGeneration,
            .sceneRevision = layer.snapshot->revision,
        });
    }
    return key;
}

} // namespace pci
