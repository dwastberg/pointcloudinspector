#include "pointcloud/PointColorPolicy.h"

#include "pointcloud/PointColorMapCatalog.h"

#include <algorithm>
#include <cmath>

namespace pci {

std::vector<PointColorSource>
availablePointColorSources(const PointCloudMetadata &metadata)
{
    std::vector<PointColorSource> sources;
    if (metadata.hasColor) {
        sources.push_back(PointColorSource::Rgb);
    }
    sources.push_back(PointColorSource::X);
    sources.push_back(PointColorSource::Y);
    sources.push_back(PointColorSource::Z);
    if (metadata.hasIntensity) {
        sources.push_back(PointColorSource::Intensity);
    }
    if (metadata.hasClassification) {
        sources.push_back(PointColorSource::Classification);
    }
    if (metadata.hasReturnNumber) {
        sources.push_back(PointColorSource::ReturnNumber);
    }
    if (metadata.hasNumberOfReturns) {
        sources.push_back(PointColorSource::NumberOfReturns);
    }
    return sources;
}

bool pointColorSourceAvailable(const std::vector<PointColorSource> &sources,
                               const PointColorSource source)
{
    return std::ranges::find(sources, source) != sources.end();
}

std::vector<PointColorMap>
availablePointColorMaps(const PointColorMapCatalogSnapshot &catalog,
                        const PointColorSource source)
{
    std::vector<PointColorMap> result;
    for (const PointColorMapDefinition &definition :
         pointColorMapCatalog(catalog)) {
        if (pointColorMapSupportsSource(catalog, definition.id, source)) {
            result.push_back(definition.id);
        }
    }
    return result;
}

bool pointColorMapAvailable(const std::vector<PointColorMap> &maps,
                            const PointColorMap map)
{
    return std::ranges::find(maps, map) != maps.end();
}

PointColorMap defaultPointColorMap(const PointColorSource source) noexcept
{
    switch (source) {
    case PointColorSource::Rgb:
        return PointColorMap::Rgb;
    case PointColorSource::X:
    case PointColorSource::Y:
    case PointColorSource::Z:
    case PointColorSource::Intensity:
        return PointColorMap::Viridis;
    case PointColorSource::Classification:
        return PointColorMap::LasClassification;
    case PointColorSource::ReturnNumber:
    case PointColorSource::NumberOfReturns:
        return PointColorMap::ReturnNumber;
    }
    return PointColorMap::Viridis;
}

PointColorMode
defaultPointColorMode(const PointCloudMetadata &metadata) noexcept
{
    const PointColorSource source =
        metadata.hasColor ? PointColorSource::Rgb : PointColorSource::Z;
    return {
        .source = source,
        .colorMap = defaultPointColorMap(source),
    };
}

std::string_view pointColorMapName(const PointColorMapCatalogSnapshot &catalog,
                                   const PointColorMap map)
{
    const PointColorMapDefinition *definition =
        pointColorMapDefinition(catalog, map);
    return definition ? definition->name : std::string_view{"Unknown"};
}

bool pointColorSourceUsesScalarRange(const PointColorSource source) noexcept
{
    switch (source) {
    case PointColorSource::X:
    case PointColorSource::Y:
    case PointColorSource::Z:
    case PointColorSource::Intensity:
        return true;
    case PointColorSource::Rgb:
    case PointColorSource::Classification:
    case PointColorSource::ReturnNumber:
    case PointColorSource::NumberOfReturns:
        return false;
    }
    return false;
}

bool validPointScalarRange(const PointScalarRange &range,
                           const bool allowDegenerate) noexcept
{
    return std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
           (allowDegenerate ? range.minimum <= range.maximum
                            : range.minimum < range.maximum);
}

bool pointColorModeAvailable(const PointColorMapCatalogSnapshot &catalog,
                             const PointCloudMetadata &metadata,
                             const PointColorMode &mode)
{
    const std::vector<PointColorSource> sources =
        availablePointColorSources(metadata);
    if (!pointColorSourceAvailable(sources, mode.source) ||
        !pointColorMapSupportsSource(catalog, mode.colorMap, mode.source)) {
        return false;
    }
    if (!mode.manualRange) {
        return true;
    }
    return pointColorSourceUsesScalarRange(mode.source) &&
           validPointScalarRange(*mode.manualRange, false);
}

std::optional<PointScalarRange>
automaticPointColorRange(const PointColorSource source,
                         const Bounds3d &documentBounds,
                         const PointCloudScalarRanges &layerRanges) noexcept
{
    switch (source) {
    case PointColorSource::X:
        return documentBounds.valid() ? std::optional<PointScalarRange>{{
                                            documentBounds.minimum[0],
                                            documentBounds.maximum[0],
                                        }}
                                      : std::nullopt;
    case PointColorSource::Y:
        return documentBounds.valid() ? std::optional<PointScalarRange>{{
                                            documentBounds.minimum[1],
                                            documentBounds.maximum[1],
                                        }}
                                      : std::nullopt;
    case PointColorSource::Z:
        return documentBounds.valid() ? std::optional<PointScalarRange>{{
                                            documentBounds.minimum[2],
                                            documentBounds.maximum[2],
                                        }}
                                      : std::nullopt;
    case PointColorSource::Intensity:
        // An unknown range must remain stable while pages enter and leave the
        // decoded cache. The full LAS domain is preferable to a residency-
        // dependent observed maximum.
        return layerRanges.intensity.value_or(PointScalarRange{0.0, 65535.0});
    case PointColorSource::Rgb:
    case PointColorSource::Classification:
    case PointColorSource::ReturnNumber:
    case PointColorSource::NumberOfReturns:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<PointScalarRange>
effectivePointColorRange(const PointColorMode &mode,
                         const Bounds3d &documentBounds,
                         const PointCloudScalarRanges &layerRanges) noexcept
{
    if (!pointColorSourceUsesScalarRange(mode.source)) {
        return std::nullopt;
    }
    if (mode.manualRange && validPointScalarRange(*mode.manualRange, false)) {
        return mode.manualRange;
    }
    return automaticPointColorRange(mode.source, documentBounds, layerRanges);
}

} // namespace pci
