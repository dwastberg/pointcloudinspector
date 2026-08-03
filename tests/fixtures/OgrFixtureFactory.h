#pragma once

#include <filesystem>

namespace pci::test {

struct OgrFixturePaths {
    std::filesystem::path geoJson;
    std::filesystem::path geoPackage;
};

// Writes a small OGR-readable corpus. The GPKG is deliberately EPSG:3006,
// since RFC 7946 GeoJSON cannot be used to exercise projected CRS handling.
[[nodiscard]] OgrFixturePaths
writeOgrFixtures(const std::filesystem::path &directory);

} // namespace pci::test
