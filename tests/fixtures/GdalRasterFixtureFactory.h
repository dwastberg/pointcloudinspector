#pragma once

#include <filesystem>
#include <vector>

namespace pci::test {

struct GdalRasterFixturePaths {
    // 64x48 Byte RGB, EPSG:3006, north-up, no overviews. Corner pixels carry
    // known colors so georeferenced placement can be asserted.
    std::filesystem::path rgb;
    // Three Byte bands with no color interpretations. Imported positionally
    // as RGB, with an explicit warning in metadata and the UI.
    std::filesystem::path positionalRgb;
    // 256x192 Byte RGB whose overviews reduce by 3 and 5, so no level ratio is
    // a power of two.
    std::filesystem::path rgbNonPowerOfTwoOverviews;
    std::filesystem::path rgba;
    // RGB with a real per-dataset validity mask.
    std::filesystem::path masked;
    std::filesystem::path gray;
    std::filesystem::path palette;
    // Float32 terrain with a nodata value and a nodata region at one edge.
    std::filesystem::path terrain;
    // UInt16 occupying only part of the 0-65535 domain.
    std::filesystem::path unsigned16;
    std::filesystem::path rotated;
    // No internal transform: placement comes only from a .tfw sidecar.
    std::filesystem::path worldFile;
    // Geographic, with an X extent wider than 180 degrees.
    std::filesystem::path antimeridian;
    // About 5000x5000 with no overviews: below rasterBoundedBaseReadPixels,
    // so it must render through the bounded base-band path.
    std::filesystem::path midSizeNoOverviews;
    // Enormous logical size, sparse on disk, no overviews.
    std::filesystem::path sparseHuge;
    // Bands drawn from two sources with different overview availability.
    std::filesystem::path mismatchedOverviews;
    // Color has a backed overview while the explicit dataset mask does not,
    // so the mask pyramid cannot satisfy that level.
    std::filesystem::path mismatchedMaskOverviews;
    // A VRT mosaic over four separate members.
    std::filesystem::path vrtMosaic;
    // A GTI catalog over the same four members, whose logical extent is fixed
    // and enormous while the members stay tiny on disk. It also has a small
    // externally backed catalog overview. Sized absolutely, not relative to
    // available memory, so CI and a workstation agree.
    std::filesystem::path catalog;
    // The catalog's members, for tests that need to read one directly.
    std::vector<std::filesystem::path> catalogMembers;
};

[[nodiscard]] GdalRasterFixturePaths
writeGdalRasterFixtures(const std::filesystem::path &directory);

} // namespace pci::test
