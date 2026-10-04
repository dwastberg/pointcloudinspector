#pragma once

#include <pci/raster/RasterIdentity.h>
#include <pci/raster/RasterLayer.h>

namespace pci {

// Immutable document-facing identity and metadata for one raster dataset.
// The pixel source remains in the runtime binding.
struct RasterDatasetDescriptor {
    RasterSourceId sourceId;
    RasterLayerMetadata metadata;
};

} // namespace pci
