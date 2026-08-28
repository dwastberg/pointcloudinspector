#include "renderer/planning/PointFrameCoordinator.h"

#include "foundation/Hash.h"
#include "pointcloud/GpuPoint.h"
#include "renderer/planning/FramePlanner.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace pci {
namespace {

constexpr std::uint64_t hierarchyRequestPointBudget = 2'000'000;

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
    hierarchySelections_.clear();
    cachedFlatPlanKey_.reset();
    cachedFlatPlan_.reset();
    flatBudgetSettlementKey_.reset();
}

PointBudgetUpdate PointFrameCoordinator::pointBudgetUpdate(
    const std::vector<PointFrameLayer> &layers,
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
    if (allFlat) {
        capacity = std::min(capacity, gpuByteBudget / sizeof(GpuPoint));
    }
    PointBudgetUpdate result{
        .total = std::max<std::uint64_t>(capacity, 1),
        .current = std::nullopt,
    };
    if (allFlatComplete) {
        FlatBudgetSettlementKey key{
            .gpuByteBudget = gpuByteBudget,
            .retainedLayerPoints = {},
        };
        key.retainedLayerPoints.reserve(layers.size());
        for (const PointFrameLayer &layer : layers) {
            key.retainedLayerPoints.emplace_back(
                layer.layer.id, layer.snapshot->retainedFlatPointCount);
        }
        if (flatBudgetSettlementKey_ != key) {
            result.current = result.total;
            flatBudgetSettlementKey_ = std::move(key);
        }
    } else {
        flatBudgetSettlementKey_.reset();
        if (allFlat) {
            result.current =
                std::max(currentPointBudget,
                         std::min<std::uint64_t>(1'000'000, result.total));
        }
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
        };
    }

    FlatFramePlanKey key = flatPlanKey(input);
    if (cachedFlatPlanKey_ == key && cachedFlatPlan_) {
        return {.plan = cachedFlatPlan_, .reused = true};
    }
    cachedFlatPlan_ = std::make_shared<const PointFramePlan>(buildPlan(input));
    cachedFlatPlanKey_ = std::move(key);
    return {.plan = cachedFlatPlan_};
}

PointFramePlan PointFrameCoordinator::buildPlan(const PointFrameInput &input)
{
    const FrameCamera &frame = input.camera;
    const FrustumCuller &culler = frame.culler;
    const auto resident = [&input](const PointFrameBlockKey &key) {
        return input.resident(key);
    };
    PointFramePlan result;
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

    std::unordered_set<PointCloudLayerId> inFrustumLayers;
    if (input.document) {
        visibilityIndex_.update(*input.document);
        const std::vector<PointCloudLayerId> intersecting =
            visibilityIndex_.visibleLayersIntersecting(
                [&culler](const Bounds3d &bounds) {
                    return culler.intersects(bounds);
                });
        inFrustumLayers.insert(intersecting.begin(), intersecting.end());
    } else {
        for (const PointFrameLayer &layer : input.layers) {
            if (!layer.snapshot->bounds.valid() ||
                culler.intersects(layer.snapshot->bounds)) {
                inFrustumLayers.insert(layer.layer.id);
            }
        }
    }

    std::vector<LayerPointBudgetCandidate> layerCandidates;
    if (!allFlat) {
        layerCandidates.reserve(inFrustumLayers.size());
        for (const PointFrameLayer &layer : input.layers) {
            if (!inFrustumLayers.contains(layer.layer.id)) {
                continue;
            }
            std::uint64_t coveragePoints = 0;
            std::uint64_t desiredPoints = 0;
            if (layer.snapshot->hierarchical) {
                const PointCloudNodePayloadPtr root =
                    layer.layer.scene->nodePayload(rootPointCloudNode);
                coveragePoints =
                    root ? pointCloudNodePayloadPoints(*root)
                         : layer.layer.scene->node(rootPointCloudNode)
                               .estimatedPointCount;
                desiredPoints = layer.snapshot->sourcePointCount;
            } else {
                desiredPoints = layer.snapshot->retainedFlatPointCount;
                for (const PointCloudSceneBlockSnapshot &entry :
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
                .layerId = layer.layer.id.value(),
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
        PointCloudScenePtr scene = layer.layer.scene;
        if (!inFrustumLayers.contains(layer.layer.id)) {
            result.outOfFrustumLayerIds.insert(layer.layer.id);
            if (layer.snapshot->hierarchical) {
                result.nodeRequests.push_back({
                    .layerId = layer.layer.id,
                    .nodes = {},
                });
                scene->requestNodes({});
                scene->trimDecodedCache();
                hierarchySelections_[layer.layer.id].reset();
                ++result.culledBlocks;
            } else {
                result.culledBlocks += layer.snapshot->flatBlocks.size();
            }
            continue;
        }
        if (layer.snapshot->hierarchical) {
            std::uint64_t layerRemaining = layerBudgets[layer.layer.id];
            if (layerRemaining == 0) {
                result.nodeRequests.push_back({
                    .layerId = layer.layer.id,
                    .nodes = {},
                });
                scene->requestNodes({});
                continue;
            }
            scene->trimDecodedCache();
            const std::string error = scene->hierarchyError();
            if (!error.empty()) {
                result.layerErrors.push_back({
                    .layerId = layer.layer.id,
                    .message = error,
                });
                result.nodeRequests.push_back({
                    .layerId = layer.layer.id,
                    .nodes = {},
                });
                scene->requestNodes({});
                hierarchySelections_.erase(layer.layer.id);
                continue;
            }
            RenderSelection &selector = hierarchySelections_[layer.layer.id];
            const std::array roots{rootPointCloudNode};
            const auto blockKey = [&layer](const PointCloudNodeId nodeId,
                                           const std::size_t index) {
                return PointFrameBlockKey{
                    .layerId = layer.layer.id,
                    .nodeId = nodeId,
                    .nodeBlockIndex = static_cast<std::uint32_t>(index),
                };
            };
            const RenderSelectionResult selection = selector.select(
                roots,
                [&scene, &blockKey, &resident](const PointCloudNodeId id) {
                    const PointCloudNodePayloadPtr payload =
                        scene->nodePayload(id);
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
                        .node = scene->node(id),
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
            scene->requestNodes(selection.requestedNodes);
            result.nodeRequests.push_back({
                .layerId = layer.layer.id,
                .nodes = selection.requestedNodes,
            });

            std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash>
                leasedNodes;
            const auto scheduleNode =
                [&result, &leasedNodes, &scene, &blockKey, &scheduleUpload](
                    const PointCloudNodeId nodeId) {
                    PointCloudNodePayloadPtr payload =
                        scene->nodePayload(nodeId);
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
                PointCloudNodePayloadPtr payload = scene->nodePayload(nodeId);
                if (!payload) {
                    continue;
                }
                const PointCloudNode node = scene->node(nodeId);
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
                        .layerId = layer.layer.id,
                        .colorMode = layer.layer.colorMode,
                        .classificationFilter =
                            layer.layer.classificationFilter,
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

        for (const PointCloudSceneBlockSnapshot &entry :
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
                .layerId = block.layer->layer.id.value(),
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
                .layerId = block.layer->layer.id,
                .nodeId = {},
                .sceneBlockId = block.sceneBlockId,
            };
            result.blocks.push_back({
                .layerId = block.layer->layer.id,
                .colorMode = block.layer->layer.colorMode,
                .classificationFilter = block.layer->layer.classificationFilter,
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
        std::uint64_t &layerRemaining = layerBudgets[block.layer->layer.id];
        if (layerRemaining == 0) {
            continue;
        }
        const auto pointCount =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                block.block->points.size(), layerRemaining));
        const PointFrameBlockKey key{
            .layerId = block.layer->layer.id,
            .nodeId = {},
            .sceneBlockId = block.sceneBlockId,
        };
        result.blocks.push_back({
            .layerId = block.layer->layer.id,
            .colorMode = block.layer->layer.colorMode,
            .classificationFilter = block.layer->layer.classificationFilter,
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
        .cameraRevision = input.cameraRevision,
        .documentRevision = input.document ? input.document->revision : 0,
        .pointBudget = input.framePointBudget,
        .outputWidth = input.camera.outputWidth,
        .outputHeight = input.camera.outputHeight,
        .sceneRevisions = {},
    };
    key.sceneRevisions.reserve(input.layers.size());
    for (const PointFrameLayer &layer : input.layers) {
        key.sceneRevisions.emplace_back(layer.layer.id,
                                        layer.snapshot->revision);
    }
    return key;
}

} // namespace pci
