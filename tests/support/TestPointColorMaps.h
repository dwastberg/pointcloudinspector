#pragma once

#include "pointcloud/PointColorMapCatalog.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace pci::test {

inline PointColorMapCatalogSnapshotPtr createTestPointColorMapCatalog()
{
    const std::vector<PointColorStop> viridisStops{
        {0.0F, {0.27F, 0.00F, 0.33F, 1.0F}},
        {1.0F, {0.99F, 0.91F, 0.14F, 1.0F}},
    };
    const std::vector<PointColorStop> turboStops{
        {0.0F, {0.19F, 0.07F, 0.23F, 1.0F}},
        {1.0F, {0.48F, 0.02F, 0.01F, 1.0F}},
    };

    PointColorMapCatalog catalog;
    const auto viridis = catalog.registerContinuous(
        "cpt:viridis.cpt",
        "Viridis",
        viridisStops,
        "A color-vision-friendly palette for general ordered values.");
    const auto turbo = catalog.registerContinuous(
        "cpt:turbo.cpt",
        "Turbo",
        turboStops,
        "A vivid high-contrast spectrum for exploratory feature separation.");
    if (!viridis || viridis.id != PointColorMap::Viridis || !turbo ||
        turbo.id != PointColorMap::Turbo) {
        throw std::logic_error("failed to create test CPT color maps");
    }
    return catalog.freeze();
}

} // namespace pci::test
