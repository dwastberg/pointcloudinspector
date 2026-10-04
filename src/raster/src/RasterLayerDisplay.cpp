#include <pci/raster/RasterLayerDisplay.h>

#include <ranges>
#include <string_view>

namespace pci {

std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayerMetadata &metadata,
                              const RasterLayerStyle &style,
                              const PointColorMapCatalogSnapshot &colorMaps)
{
    auto decode = std::make_shared<RasterDecodeParameters>();
    *decode = metadata.defaultDisplay;
    if (style.displayRange) {
        decode->displayRange = style.displayRange;
    }
    if (decode->sampleKind != RasterSampleKind::ContinuousScalar ||
        style.colorRampKey.empty()) {
        return decode;
    }

    const auto definition =
        std::ranges::find(colorMaps.definitions(),
                          std::string_view(style.colorRampKey),
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
