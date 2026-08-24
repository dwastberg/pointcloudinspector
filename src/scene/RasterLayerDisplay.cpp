#include "scene/RasterLayerDisplay.h"

#include <ranges>
#include <string_view>

namespace pci {

std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayer &layer,
                              const PointColorMapCatalogSnapshot &colorMaps)
{
    auto decode = std::make_shared<RasterDecodeParameters>();
    if (!layer.data) {
        return decode;
    }
    *decode = layer.data->metadata().defaultDisplay;
    if (layer.style.displayRange) {
        decode->displayRange = layer.style.displayRange;
    }
    if (decode->sampleKind != RasterSampleKind::ContinuousScalar ||
        layer.style.colorRampKey.empty()) {
        return decode;
    }

    const auto definition =
        std::ranges::find(colorMaps.definitions(),
                          std::string_view(layer.style.colorRampKey),
                          &PointColorMapDefinition::key);
    if (definition != colorMaps.definitions().end() &&
        definition->kind == PointColorMapKind::Continuous &&
        !definition->stops.empty()) {
        decode->colorRamp = std::make_shared<const std::vector<PointColorStop>>(
            definition->stops.begin(), definition->stops.end());
    }
    return decode;
}

} // namespace pci
