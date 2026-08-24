#pragma once

#include "pointcloud/PointCloudMetadata.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pci {

class PointColorMapCatalogSnapshot;

enum class PointColorSource : int {
    Rgb = 0,
    X = 1,
    Y = 2,
    Z = 3,
    Intensity = 4,
    Classification = 5,
    ReturnNumber = 6,
    NumberOfReturns = 7,
};

enum class PointColorMap : int {
    Rgb = 0,
    // Stable IDs retained for the CPT-backed palettes used as defaults and by
    // persisted selections. Their color data is not built into the program.
    Viridis = 2,
    Turbo = 3,
    LasClassification = 4,
    ReturnNumber = 5,
};

struct PointScalarRange {
    double minimum = 0.0;
    double maximum = 1.0;

    bool operator==(const PointScalarRange &) const = default;
};

struct PointCloudScalarRanges {
    std::optional<PointScalarRange> intensity;

    bool operator==(const PointCloudScalarRanges &) const = default;
};

struct PointColorMode {
    PointColorSource source = PointColorSource::Rgb;
    PointColorMap colorMap = PointColorMap::Rgb;
    // No value means "Auto". Manual ranges are meaningful only for the
    // continuous X/Y/Z and intensity sources.
    std::optional<PointScalarRange> manualRange;

    bool operator==(const PointColorMode &) const = default;
};

[[nodiscard]] std::vector<PointColorSource>
availablePointColorSources(const PointCloudMetadata &metadata,
                           bool hasOverrideColor = false);
[[nodiscard]] bool
pointColorSourceAvailable(const std::vector<PointColorSource> &sources,
                          PointColorSource source);
[[nodiscard]] std::vector<PointColorMap>
availablePointColorMaps(const PointColorMapCatalogSnapshot &catalog,
                        PointColorSource source);
[[nodiscard]] bool
pointColorMapAvailable(const std::vector<PointColorMap> &maps,
                       PointColorMap map);
[[nodiscard]] PointColorMap
defaultPointColorMap(const PointColorMapCatalogSnapshot &catalog,
                     PointColorSource source) noexcept;
[[nodiscard]] PointColorMode
defaultPointColorMode(const PointColorMapCatalogSnapshot &catalog,
                      const PointCloudMetadata &metadata) noexcept;

[[nodiscard]] std::string_view
pointColorMapName(const PointColorMapCatalogSnapshot &catalog,
                  PointColorMap map);
[[nodiscard]] std::string_view
pointColorMapDescription(const PointColorMapCatalogSnapshot &catalog,
                         PointColorMap map);
[[nodiscard]] bool
pointColorSourceUsesScalarRange(PointColorSource source) noexcept;
[[nodiscard]] bool validPointScalarRange(const PointScalarRange &range,
                                         bool allowDegenerate = true) noexcept;
[[nodiscard]] bool
pointColorModeAvailable(const PointColorMapCatalogSnapshot &catalog,
                        const PointCloudMetadata &metadata,
                        const PointColorMode &mode,
                        bool hasOverrideColor = false);

// X/Y/Z deliberately use the bounds of every layer in the document. This
// gives equal world coordinates equal colors across sources and prevents
// visibility toggles from recoloring the scene. Intensity remains per-source.
[[nodiscard]] std::optional<PointScalarRange>
automaticPointColorRange(PointColorSource source,
                         const Bounds3d &documentBounds,
                         const PointCloudScalarRanges &layerRanges) noexcept;
[[nodiscard]] std::optional<PointScalarRange>
effectivePointColorRange(const PointColorMode &mode,
                         const Bounds3d &documentBounds,
                         const PointCloudScalarRanges &layerRanges) noexcept;

} // namespace pci
