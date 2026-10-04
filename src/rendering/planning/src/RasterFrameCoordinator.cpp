#include <pci/rendering/planning/RasterFrameCoordinator.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace pci {

RasterRequestBatch
RasterFrameRequestPlan::asBatch(RasterTileSourcePtr source) const noexcept
{
    return {
        .sourceId = sourceId,
        .bindingGeneration = bindingGeneration,
        .renderGeneration = renderGeneration,
        .source = std::move(source),
        .decode = decode,
        .profile = profile,
        .orderedRequests = orderedRequests,
    };
}

RasterFramePlan RasterFrameCoordinator::buildPlan(const RasterFrameInput &input)
{
    RasterFramePlan frame;
    frame.layers.reserve(input.layers.size());
    frame.requests.reserve(input.layers.size());
    frame.retainedLayers.reserve(input.layers.size());
    frame.liveSources.reserve(input.layers.size());
    std::uint32_t finestLevel = std::numeric_limits<std::uint32_t>::max();

    bool elevationPayloads = false;
    for (const RasterFrameLayerInput &layer : input.layers) {
        if (!layer.visible || !layer.sourceAvailable) {
            continue;
        }
        ++frame.statistics.visibleLayers;
        elevationPayloads =
            elevationPayloads || layer.layer.metadata.elevation.available;
    }
    const std::size_t frameTileCapacity =
        input.surfaceSupported && elevationPayloads
            ? input.elevationTileCapacity
            : input.colorTileCapacity;
    const std::size_t perLayerTileCapacity =
        frame.statistics.visibleLayers == 0
            ? 1
            : std::max<std::size_t>(
                  1, frameTileCapacity / frame.statistics.visibleLayers);

    for (std::size_t index = 0; index < input.layers.size(); ++index) {
        const RasterFrameLayerInput &layer = input.layers[index];
        frame.retainedLayers.push_back(layer.layerId);
        if (!layer.sourceAvailable) {
            continue;
        }

        frame.liveSources.push_back(layer.sourceId);
        frame.uploadTargets.insert_or_assign(
            layer.sourceId,
            RasterFrameUploadTarget{
                .layerId = layer.layerId,
                .nearest = layer.layer.metadata.defaultDisplay.sampleKind ==
                           RasterSampleKind::Categorical,
            });

        const RasterTilePayloadProfile profile =
            input.surfaceSupported && layer.layer.metadata.elevation.available
                ? RasterTilePayloadProfile::RenderElevation
                : RasterTilePayloadProfile::ColorOnly;
        RasterFrameRequestPlan &request =
            frame.requests.emplace_back(RasterFrameRequestPlan{
                .sourceId = layer.sourceId,
                .bindingGeneration = layer.bindingGeneration,
                .renderGeneration = layer.renderGeneration,
                .decode = layer.decode,
                .profile = profile,
                .orderedRequests = {},
            });
        if (!layer.visible) {
            continue;
        }

        RasterLodPlanInput lodInput{layer.layer, input.camera};
        if (const auto previous = previousSelections_.find(layer.layerId);
            previous != previousSelections_.end() &&
            previous->second.sourceId == layer.sourceId &&
            previous->second.bindingGeneration == layer.bindingGeneration) {
            lodInput.previousSelection = previous->second.tiles;
        }
        lodInput.gpuCapacityTiles = perLayerTileCapacity;
        const auto cacheKey = [&layer, profile](const RasterTileKey key) {
            return RasterCacheKey{
                layer.sourceId, layer.renderGeneration, key, profile};
        };
        lodInput.cpuResident = [&input, &cacheKey](const RasterTileKey key) {
            return input.cpuResident && input.cpuResident(cacheKey(key));
        };
        lodInput.gpuResident = [&input, &cacheKey](const RasterTileKey key) {
            return input.gpuResident && input.gpuResident(cacheKey(key));
        };
        lodInput.unavailable = [&input, &cacheKey](const RasterTileKey key) {
            return input.unavailable && input.unavailable(cacheKey(key));
        };

        RasterLodPlan lod = planRasterTiles(lodInput);
        request.orderedRequests = std::move(lod.requests);
        frame.statistics.selectedTiles += lod.selected.size();
        frame.statistics.coverageIncomplete =
            frame.statistics.coverageIncomplete || lod.coverageIncomplete;
        for (const RasterTileKey key : lod.selected) {
            finestLevel = std::min(finestLevel, key.levelIndex);
            frame.statistics.coarsestLevel =
                std::max(frame.statistics.coarsestLevel, key.levelIndex);
        }
        for (const RasterTileKey key : lod.protectedTiles) {
            frame.protectedTiles.push_back(cacheKey(key));
        }
        for (const RasterTileKey key : lod.decodedUploads) {
            frame.decodedUploads.push_back(cacheKey(key));
        }

        frame.layers.push_back(RasterFrameLayerPlan{
            .inputIndex = index,
            .profile = profile,
            .surface =
                input.surfaceSupported &&
                rasterEffectiveRenderMode(layer.layer.metadata,
                                          layer.layer.style,
                                          layer.layer.elevationStatus,
                                          layer.layer.exactElevationRange) ==
                    RasterRenderMode::Surface,
            .selected = std::move(lod.selected),
            .draw = std::move(lod.draw),
        });
    }

    frame.statistics.finestLevel =
        finestLevel == std::numeric_limits<std::uint32_t>::max() ? 0
                                                                 : finestLevel;

    std::erase_if(previousSelections_, [&frame](const auto &entry) {
        return std::ranges::find(frame.retainedLayers, entry.first) ==
               frame.retainedLayers.end();
    });
    for (const RasterFrameLayerPlan &layer : frame.layers) {
        const RasterFrameLayerInput &source = input.layers[layer.inputIndex];
        previousSelections_.insert_or_assign(
            source.layerId,
            PreviousSelection{
                .sourceId = source.sourceId,
                .bindingGeneration = source.bindingGeneration,
                .tiles = layer.selected,
            });
    }
    return frame;
}

void RasterFrameCoordinator::clear() noexcept
{
    previousSelections_.clear();
}

} // namespace pci
