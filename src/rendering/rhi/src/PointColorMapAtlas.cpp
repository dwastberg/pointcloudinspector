#include <pci/rendering/rhi/PointColorMapAtlas.h>

#include <pci/color/PointColorMapCatalog.h>

#include <QColor>

#include <algorithm>
#include <cstdint>

namespace pci {
namespace {

QColor toQColor(const PointRgba color)
{
    return QColor::fromRgbF(std::clamp(color.red, 0.0F, 1.0F),
                            std::clamp(color.green, 0.0F, 1.0F),
                            std::clamp(color.blue, 0.0F, 1.0F),
                            std::clamp(color.alpha, 0.0F, 1.0F));
}

std::size_t mapIndex(const PointColorMapCatalogSnapshot &snapshot,
                     const PointColorMap map)
{
    const std::span catalog = pointColorMapCatalog(snapshot);
    const auto found =
        std::ranges::find(catalog, map, &PointColorMapDefinition::id);
    return found == catalog.end()
               ? 0
               : static_cast<std::size_t>(found - catalog.begin());
}

} // namespace

QImage buildPointColorMapAtlas(const PointColorMapCatalogSnapshot &snapshot)
{
    const std::span catalog = pointColorMapCatalog(snapshot);
    QImage image(pointColorMapAtlasWidth,
                 static_cast<int>(catalog.size()) *
                     pointColorMapAtlasRowsPerMap,
                 QImage::Format_RGBA8888);
    image.fill(Qt::transparent);

    for (std::size_t row = 0; row < catalog.size(); ++row) {
        const PointColorMapDefinition &definition = catalog[row];
        for (int sample = 0; sample < pointColorMapAtlasWidth; ++sample) {
            PointRgba color;
            switch (definition.kind) {
            case PointColorMapKind::Direct:
                color = {1.0F, 1.0F, 1.0F, 1.0F};
                break;
            case PointColorMapKind::Continuous:
                color = sampleContinuousPointColorMap(
                    snapshot,
                    definition.id,
                    static_cast<float>(sample) /
                        static_cast<float>(pointColorMapAtlasWidth - 1));
                break;
            case PointColorMapKind::Categorical:
                color = sampleCategoricalPointColorMap(
                    snapshot, definition.id, static_cast<std::uint8_t>(sample));
                break;
            }
            const QColor pixel = toQColor(color);
            for (int padding = 0; padding < pointColorMapAtlasRowsPerMap;
                 ++padding) {
                image.setPixelColor(sample,
                                    static_cast<int>(row) *
                                            pointColorMapAtlasRowsPerMap +
                                        padding,
                                    pixel);
            }
        }
    }
    return image;
}

PointColorMapSampling
pointColorMapSampling(const PointColorMapCatalogSnapshot &snapshot,
                      const PointColorMap map)
{
    const std::size_t count = pointColorMapCatalog(snapshot).size();
    if (count == 0) {
        return {};
    }
    const float height =
        static_cast<float>(count * pointColorMapAtlasRowsPerMap);
    const float center =
        static_cast<float>(
            mapIndex(snapshot, map) * pointColorMapAtlasRowsPerMap + 1) +
        0.5F;
    return {
        .rowCoordinate = center / height,
    };
}

} // namespace pci
