#pragma once

#include <filesystem>

namespace pci::test {

struct GdalRasterFixturePaths {
    // 64x48 Byte RGB, EPSG:3006, north-up, no overviews. Corner pixels carry
    // known colors so georeferenced placement can be asserted.
    std::filesystem::path rgb;
    // 256x192 Byte RGB whose overviews reduce by 3 and 5, so no level ratio is
    // a power of two.
    std::filesystem::path rgbNonPowerOfTwoOverviews;
    std::filesystem::path rgba;
    std::filesystem::path gray;
    std::filesystem::path palette;
    // Float32 terrain with a nodata value and a nodata region at one edge.
    std::filesystem::path terrain;
    // UInt16 occupying only part of the 0-65535 domain.
    std::filesystem::path unsigned16;
    std::filesystem::path rotated;
    // Geographic, with an X extent wider than 180 degrees.
    std::filesystem::path antimeridian;
    // Enormous logical size, sparse on disk, no overviews.
    std::filesystem::path sparseHuge;
    // Bands drawn from two sources with different overview availability.
    std::filesystem::path mismatchedOverviews;
};

[[nodiscard]] GdalRasterFixturePaths
writeGdalRasterFixtures(const std::filesystem::path &directory);

} // namespace pci::test
