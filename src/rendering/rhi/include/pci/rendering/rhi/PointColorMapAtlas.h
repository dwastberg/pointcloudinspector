#pragma once

#include <pci/pointcloud/PointColorPolicy.h>

#include <pci/color/PointColorMapCatalog.h>

#include <QImage>

namespace pci {

inline constexpr int pointColorMapAtlasWidth = 256;
inline constexpr int pointColorMapAtlasRowsPerMap = 3;

struct PointColorMapSampling {
    float rowCoordinate = 0.5F;
    float inverseWidth = 1.0F / pointColorMapAtlasWidth;
    float normalizedSpan = static_cast<float>(pointColorMapAtlasWidth - 1) /
                           static_cast<float>(pointColorMapAtlasWidth);
};

[[nodiscard]] QImage
buildPointColorMapAtlas(const PointColorMapCatalogSnapshot &catalog);
[[nodiscard]] PointColorMapSampling
pointColorMapSampling(const PointColorMapCatalogSnapshot &catalog,
                      PointColorMap map);

} // namespace pci
