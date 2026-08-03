#include "renderer/planning/FullDetailController.h"

#include "renderer/planning/RenderSelection.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

void saturatingAdd(std::uint64_t &value, const std::uint64_t increment) noexcept
{
    value = increment > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + increment;
}

} // namespace

FullDetailController::~FullDetailController()
{
    resetPlan();
}

FullDetailConfigurationChange
FullDetailController::configure(FullDetailConfiguration configuration)
{
    decisionPending_ = false;
    const bool invalidShape =
        configuration.visibleLayers.empty() ||
        std::ranges::any_of(configuration.visibleLayers,
                            [](const FullDetailLayerDescriptor &layer) {
                                return !layer.hierarchical;
                            });
    if (invalidShape) {
        return clear();
    }

    if (plan_ && plan_->documentRevision == configuration.documentRevision &&
        plan_->decodedByteBudget == configuration.decodedByteBudget &&
        plan_->gpuByteBudget == configuration.gpuByteBudget &&
        plan_->layers.size() == configuration.visibleLayers.size() &&
        std::ranges::equal(
            plan_->layers,
            configuration.visibleLayers,
            {},
            [](const LayerPlan &layer) {
                return layer.descriptor.layerId;
            },
            &FullDetailLayerDescriptor::layerId)) {
        return {};
    }

    std::vector<LayerPlan> layers;
    std::vector<FullDetailResidencyCandidate> candidates;
    bool waitingForCompletedSource = false;
    bool hasUnsupportedSource = false;
    layers.reserve(configuration.visibleLayers.size());
    candidates.reserve(configuration.visibleLayers.size());
    for (FullDetailLayerDescriptor &layer : configuration.visibleLayers) {
        if (!layer.detail) {
            if (layer.loadingComplete) {
                hasUnsupportedSource = true;
            } else {
                waitingForCompletedSource = true;
            }
            continue;
        }
        candidates.push_back({
            .sourcePointCount = layer.sourcePointCount,
            .detailPointCount = layer.detail->pointCount,
            .decodedBytes = layer.detail->decodedBytes,
            .decodedByteBudget = layer.decodedByteBudget,
            .gpuBytes = layer.detail->gpuBytes,
        });
        PointCloudFullDetailInfo detail = *layer.detail;
        layers.push_back({
            .descriptor = std::move(layer),
            .detail = std::move(detail),
        });
    }

    const bool hadPlan = plan_.has_value();
    resetPlan();
    if (hasUnsupportedSource || waitingForCompletedSource) {
        decisionPending_ = !hasUnsupportedSource && waitingForCompletedSource;
        return {.planStopped = hadPlan};
    }
    if (!fullDetailResidencyFits(candidates,
                                 configuration.decodedByteBudget,
                                 configuration.gpuByteBudget)) {
        return {.planStopped = hadPlan};
    }

    plan_ = Plan{
        .documentRevision = configuration.documentRevision,
        .decodedByteBudget = configuration.decodedByteBudget,
        .gpuByteBudget = configuration.gpuByteBudget,
        .layers = std::move(layers),
    };
    for (const LayerPlan &layer : plan_->layers) {
        layer.descriptor.setPinnedNodes(layer.detail.leafNodes);
    }
    return {.planStarted = true, .planStopped = hadPlan};
}

FullDetailConfigurationChange FullDetailController::clear()
{
    const bool hadPlan = plan_.has_value();
    resetPlan();
    decisionPending_ = false;
    return {.planStopped = hadPlan};
}

FullDetailFrameResult
FullDetailController::advance(const VisibilityQuery &visible,
                              const ResidencyQuery &resident)
{
    FullDetailFrameResult result;
    if (!plan_) {
        return result;
    }

    bool ready = true;
    std::vector<FullDetailBlock> leafDrawables;
    for (LayerPlan &layer : plan_->layers) {
        const std::size_t leafCount = layer.detail.leafNodes.size();
        if (leafCount == 0) {
            continue;
        }
        std::size_t inspected = 0;
        std::size_t probed = 0;
        while (inspected < leafCount && probed < maximumNodeProbesPerFrame) {
            const PointCloudNodeId nodeId =
                layer.detail.leafNodes[layer.probeCursor];
            layer.probeCursor = (layer.probeCursor + 1U) % leafCount;
            ++inspected;
            if (layer.decodedPayloads.contains(nodeId)) {
                continue;
            }
            ++probed;
            if (PointCloudNodePayloadPtr payload =
                    layer.descriptor.peekPayload(nodeId)) {
                layer.decodedPayloads.emplace(nodeId, std::move(payload));
            }
        }
        if (layer.decodedPayloads.size() != leafCount) {
            ready = false;
            const std::string error = layer.descriptor.hierarchyError();
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
            if (!layer.descriptor.decodeInFlight() && inspected == leafCount) {
                std::vector<PointCloudNodeId> missing;
                missing.reserve(leafCount - layer.decodedPayloads.size());
                for (const PointCloudNodeId nodeId : layer.detail.leafNodes) {
                    if (!layer.decodedPayloads.contains(nodeId)) {
                        missing.push_back(nodeId);
                    }
                }
                if (layer.recoveryAttempts >= maximumRecoveryAttempts) {
                    throw std::runtime_error(
                        "full-detail residency could not recover " +
                        std::to_string(missing.size()) +
                        " missing hierarchy page(s)");
                }
                ++layer.recoveryAttempts;
                result.nodeRequests.push_back({
                    .layerId = layer.descriptor.layerId,
                    .nodes = std::move(missing),
                });
            }
            result.requiresContinuation = result.requiresContinuation ||
                                          probed == maximumNodeProbesPerFrame;
        }

        for (const PointCloudNodeId nodeId : layer.detail.leafNodes) {
            const auto decoded = layer.decodedPayloads.find(nodeId);
            if (decoded == layer.decodedPayloads.end()) {
                continue;
            }
            const PointCloudNodePayloadPtr &payload = decoded->second;
            result.payloadLeases.push_back(payload);
            const bool visibleNode =
                visible(layer.descriptor.nodeBounds(nodeId));
            for (std::size_t index = 0; index < payload->blocks.size();
                 ++index) {
                const PointBlockPtr &block = payload->blocks[index];
                const FullDetailBlockId id{
                    .layerId = layer.descriptor.layerId,
                    .nodeId = nodeId,
                    .nodeBlockIndex = static_cast<std::uint32_t>(index),
                };
                const FullDetailBlock candidate{
                    .id = id,
                    .block = block,
                    .pointCount =
                        static_cast<std::uint32_t>(block->points.size()),
                };
                result.uploadRequests.push_back(candidate);
                result.protectedBlocks.push_back(id);
                if (!resident(id)) {
                    ready = false;
                }
                if (!visibleNode) {
                    ++result.culledBlocks;
                    continue;
                }
                leafDrawables.push_back(candidate);
                saturatingAdd(result.selectedPoints, candidate.pointCount);
                ++result.visibleBlocks;
            }
        }
    }

    plan_->active = ready;
    result.active = ready;
    if (ready) {
        result.drawableBlocks = std::move(leafDrawables);
        return result;
    }

    result.selectedPoints = 0;
    result.visibleBlocks = 0;
    result.culledBlocks = 0;
    for (const LayerPlan &layer : plan_->layers) {
        PointCloudNodePayloadPtr root = layer.descriptor.rootPayload();
        if (!root) {
            continue;
        }
        result.payloadLeases.push_back(root);
        for (std::size_t index = 0; index < root->blocks.size(); ++index) {
            const PointBlockPtr &block = root->blocks[index];
            const FullDetailBlockId id{
                .layerId = layer.descriptor.layerId,
                .nodeId = rootPointCloudNode,
                .nodeBlockIndex = static_cast<std::uint32_t>(index),
            };
            const FullDetailBlock candidate{
                .id = id,
                .block = block,
                .pointCount = static_cast<std::uint32_t>(block->points.size()),
            };
            result.uploadRequests.push_back(candidate);
            result.protectedBlocks.push_back(id);
            if (!resident(id)) {
                continue;
            }
            result.drawableBlocks.push_back(candidate);
            saturatingAdd(result.selectedPoints, candidate.pointCount);
            ++result.visibleBlocks;
        }
    }
    return result;
}

std::vector<FullDetailProgressUpdate>
FullDetailController::progressUpdates(const ResidencyQuery &resident)
{
    std::vector<FullDetailProgressUpdate> updates;
    if (!plan_ || plan_->active) {
        return updates;
    }
    for (const LayerPlan &layer : plan_->layers) {
        Progress progress;
        for (const auto &[nodeId, payload] : layer.decodedPayloads) {
            for (std::size_t index = 0; index < payload->blocks.size();
                 ++index) {
                const std::uint64_t points =
                    payload->blocks[index]->points.size();
                saturatingAdd(progress.decoded, points);
                if (resident({.layerId = layer.descriptor.layerId,
                              .nodeId = nodeId,
                              .nodeBlockIndex =
                                  static_cast<std::uint32_t>(index)})) {
                    saturatingAdd(progress.uploaded, points);
                }
            }
        }
        const auto previous = publishedProgress_.find(layer.descriptor.layerId);
        if (previous != publishedProgress_.end() &&
            previous->second == progress) {
            continue;
        }
        publishedProgress_.insert_or_assign(layer.descriptor.layerId, progress);
        updates.push_back({
            .layerId = layer.descriptor.layerId,
            .decoded = progress.decoded,
            .uploaded = progress.uploaded,
            .total = layer.detail.pointCount,
        });
    }
    return updates;
}

bool FullDetailController::hasPlan() const noexcept
{
    return plan_.has_value();
}

bool FullDetailController::active() const noexcept
{
    return plan_ && plan_->active;
}

bool FullDetailController::decisionPending() const noexcept
{
    return decisionPending_;
}

FullDetailStatus FullDetailController::status() const
{
    FullDetailStatus result{
        .warming = plan_.has_value() && !plan_->active,
        .active = plan_.has_value() && plan_->active,
        .decisionPending = decisionPending_,
    };
    if (!plan_) {
        return result;
    }
    for (const LayerPlan &layer : plan_->layers) {
        saturatingAdd(result.decodedNodes, layer.decodedPayloads.size());
        saturatingAdd(result.totalNodes, layer.detail.leafNodes.size());
        if (layer.descriptor.decodeInFlight()) {
            ++result.decodesInFlight;
        }
    }
    return result;
}

std::optional<FullDetailLayerStatus>
FullDetailController::layerStatus(const PointCloudLayerId layerId) const
{
    if (!plan_) {
        return std::nullopt;
    }
    const auto layer = std::ranges::find_if(
        plan_->layers, [layerId](const LayerPlan &candidate) {
            return candidate.descriptor.layerId == layerId;
        });
    if (layer == plan_->layers.end()) {
        return std::nullopt;
    }
    return FullDetailLayerStatus{
        .decodedNodes = layer->decodedPayloads.size(),
        .totalNodes = layer->detail.leafNodes.size(),
        .pointCount = layer->detail.pointCount,
        .decodeInFlight = layer->descriptor.decodeInFlight(),
    };
}

void FullDetailController::resetPlan()
{
    publishedProgress_.clear();
    if (!plan_) {
        return;
    }
    for (const LayerPlan &layer : plan_->layers) {
        layer.descriptor.setPinnedNodes({});
    }
    plan_.reset();
}

} // namespace pci
