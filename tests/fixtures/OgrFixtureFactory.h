#pragma once

#include <filesystem>

class OGRLayer;
class OGRGeometry;

namespace pci::test {

// Copies geometry into a feature and reports fixture setup failures.
void writeOgrFixtureFeature(OGRLayer *layer, const OGRGeometry &geometry);

struct OgrFixturePaths {
    std::filesystem::path geoJson;
    std::filesystem::path geoPackage;
};

// Writes a small OGR-readable corpus. The GPKG is deliberately EPSG:3006,
// since RFC 7946 GeoJSON cannot be used to exercise projected CRS handling.
[[nodiscard]] OgrFixturePaths
writeOgrFixtures(const std::filesystem::path &directory);

} // namespace pci::test
