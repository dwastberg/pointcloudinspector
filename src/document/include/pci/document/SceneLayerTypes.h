#pragma once

#include <pci/foundation/LayerIdentity.h>
#include <pci/foundation/SpatialReferenceComparator.h>
#include <pci/raster/RasterIdentity.h>
#include <pci/raster/RasterPointColorBinding.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace pci {

struct RasterDecodeParameters;

enum class SceneLayerKind : std::uint8_t {
    None = 0,
    PointCloud = 1,
    Vector = 2,
    Raster = 3
};

} // namespace pci
