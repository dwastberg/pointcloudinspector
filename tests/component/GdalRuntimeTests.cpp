#include <pci/adapters/gdal/runtime/GdalRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>

namespace {

TEST_CASE("GDAL registration reports the supported version floor",
          "[component][gdal]")
{
    pci::ensureGdalRegistered();
    pci::ensureGdalRegistered(); // registration is idempotent

    const pci::GdalRuntimeVersion version = pci::gdalRuntimeVersion();
    CHECK_FALSE(version.release.empty());
    CHECK(version.major >= 3);
    if (version.major == 3) {
        CHECK(version.minor >= 9);
    }
}

TEST_CASE("GDAL driver availability is probed, not inferred",
          "[component][gdal]")
{
    // A version floor is not a driver guarantee, so the drivers this feature
    // depends on are measured against the build that actually shipped.
    CHECK(pci::gdalDriverAvailable("GTiff"));
    CHECK(pci::gdalDriverAvailable("VRT"));
    CHECK(pci::gdalDriverAvailable("GTI"));

    CHECK_FALSE(pci::gdalDriverAvailable("NoSuchDriverExists"));
    CHECK_FALSE(pci::gdalDriverAvailable(""));
}

TEST_CASE("GDAL catalog capabilities are measured once", "[component][gdal]")
{
    const pci::GdalCatalogCapabilities &capabilities =
        pci::gdalCatalogCapabilities();

    // GTI plus at least one index format is what the supported lane promises,
    // and this assertion is the thing that fails if a packaged build drops to
    // a stripped GDAL.
    CHECK(capabilities.tileIndex);
    CHECK(capabilities.virtualRaster);
    CHECK(capabilities.catalogImport());
    CHECK((capabilities.geoPackage || capabilities.flatGeobuf ||
           capabilities.shapefile));
    CHECK(capabilities.version.major >= 3);

    // Repeated calls return the same probe rather than re-querying GDAL.
    CHECK(&capabilities == &pci::gdalCatalogCapabilities());
}

TEST_CASE("catalog capability requires an index format", "[unit][gdal]")
{
    // GTI alone cannot open a catalog whose index lives in a format the build
    // lacks, so the capability is the conjunction rather than the GTI probe.
    pci::GdalCatalogCapabilities capabilities;
    capabilities.tileIndex = true;
    CHECK_FALSE(capabilities.catalogImport());

    capabilities.flatGeobuf = true;
    CHECK(capabilities.catalogImport());

    capabilities.tileIndex = false;
    CHECK_FALSE(capabilities.catalogImport());
}

TEST_CASE("GDAL block cache limit is explicit and observable",
          "[component][gdal]")
{
    const std::uint64_t original = pci::gdalBlockCacheBytes();

    constexpr std::uint64_t requested = 64ULL * 1024ULL * 1024ULL;
    pci::setGdalBlockCacheBytes(requested);
    CHECK(pci::gdalBlockCacheBytes() == requested);

    // Usage is reported from the same allocator the limit governs, which is
    // what lets diagnostics show the term the application's own byte counters
    // cannot see.
    CHECK(pci::gdalBlockCacheUsedBytes() <= pci::gdalBlockCacheBytes());

    pci::setGdalBlockCacheBytes(original);
    CHECK(pci::gdalBlockCacheBytes() == original);
}

TEST_CASE("GDAL error scope starts empty and nests", "[component][gdal]")
{
    const pci::GdalErrorScope outer;
    CHECK(outer.message().empty());
    {
        const pci::GdalErrorScope inner;
        CHECK(inner.message().empty());
    }
    CHECK(outer.message().empty());
}

TEST_CASE("GDAL shared open helper owns errors and path formatting",
          "[component][gdal][ownership]")
{
    const std::filesystem::path missing =
        "path-that-does-not-exist/source.gpkg";
    const pci::GdalDatasetOpenResult raster =
        pci::tryOpenGdalDataset(missing, pci::GdalDatasetKind::Raster);
    CHECK_FALSE(raster.dataset);
    CHECK_FALSE(raster.error.empty());

    const pci::GdalDatasetOpenResult vector =
        pci::tryOpenGdalDataset(missing, pci::GdalDatasetKind::Vector);
    CHECK_FALSE(vector.dataset);
    CHECK_FALSE(vector.error.empty());

    CHECK(pci::gdalOpenFailureMessage("vector", missing, "driver detail") ==
          "Could not open vector source 'path-that-does-not-exist/"
          "source.gpkg': driver detail");
    CHECK(pci::gdalOpenFailureMessage("raster", missing) ==
          "Could not open raster source 'path-that-does-not-exist/"
          "source.gpkg'");
}

} // namespace
