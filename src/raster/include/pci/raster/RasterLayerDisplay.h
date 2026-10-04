#pragma once

#include <pci/color/PointColorMapCatalog.h>
#include <pci/raster/RasterLayer.h>

#include <memory>

namespace pci {

// Combines immutable source defaults with the layer style for one render
// generation. The returned parameters remain valid for asynchronous reads.
[[nodiscard]] std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayerMetadata &metadata,
                              const RasterLayerStyle &style,
                              const PointColorMapCatalogSnapshot &colorMaps);

} // namespace pci
