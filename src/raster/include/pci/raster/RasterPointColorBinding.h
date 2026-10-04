#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <pci/foundation/LayerIdentity.h>
#include <pci/foundation/SpatialReferenceComparator.h>
#include <pci/raster/RasterIdentity.h>
namespace pci {
struct RasterDecodeParameters;
struct RasterPointColorBinding {
    std::optional<SceneLayerId> rasterLayerId;
    RasterSourceId rasterSourceId;
    std::filesystem::path rasterSourcePath;
    std::shared_ptr<const RasterDecodeParameters> decode;
    std::uint64_t rasterRenderGeneration = 0;
    std::uint64_t coloredPoints = 0;
    std::uint64_t uncoloredPoints = 0;
    std::optional<SpatialReferenceRelation> crsRelation;
};
} // namespace pci
