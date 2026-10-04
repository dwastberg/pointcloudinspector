#pragma once

#include <QString>

#include <cstddef>
#include <vector>

namespace pci {

class PointColorMapCatalog;

struct EmbeddedColorMapIssue {
    QString resourcePath;
    std::size_t line = 0;
    QString message;
};

struct EmbeddedColorMapLoadResult {
    std::size_t loadedMapCount = 0;
    std::vector<EmbeddedColorMapIssue> issues;
};

// Loads every .cpt file embedded below :/colormaps. Call this once during
// startup, before constructing UI or renderer objects that read the catalog.
[[nodiscard]] EmbeddedColorMapLoadResult
loadEmbeddedColorMaps(PointColorMapCatalog &catalog);

} // namespace pci
