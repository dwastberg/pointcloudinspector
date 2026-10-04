#pragma once

#include <pci/foundation/Bounds3d.h>
#include <pci/foundation/Generation.h>
#include <pci/raster/RasterTileSource.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace pci {

// Range sampling is reported separately because it is the only part of
// inspection whose cost depends on the source rather than on its header.
enum class RasterImportPhase : std::uint8_t {
    Inspecting,
    SamplingRange,
};

struct RasterImportRequest {
    std::filesystem::path sourcePath;
    // Compared against, never reprojected to. A mismatch is a warning on the
    // layer rather than a transformation.
    std::string targetSpatialReferenceWkt;
    std::optional<Bounds3d> targetExtent;
    std::stop_token stopToken;
    std::function<void(RasterImportPhase)> phase;
    SessionGeneration session = {};
};

struct RasterImportPreflight {
    RasterLayerDataPtr data;
};

// Inspection is asynchronous and reads no full-resolution pixel payload. It
// may perform a strictly bounded window sample to derive a display range.
class RasterLoader {
public:
    RasterLoader() = default;
    virtual ~RasterLoader() = default;
    RasterLoader(const RasterLoader &) = delete;
    RasterLoader &operator=(const RasterLoader &) = delete;
    RasterLoader(RasterLoader &&) = delete;
    RasterLoader &operator=(RasterLoader &&) = delete;

    [[nodiscard]] virtual RasterImportPreflight
    inspect(const RasterImportRequest &request) const = 0;
};

} // namespace pci
