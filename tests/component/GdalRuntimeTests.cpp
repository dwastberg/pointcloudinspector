#include <pci/adapters/gdal/runtime/GdalRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <cpl_error.h>

#include <cstdint>
#include <filesystem>

namespace {

TEST_CASE("GDAL runtime provides the supported import capabilities",
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
    // A version floor is not a driver guarantee, so the drivers this feature
    // depends on are measured against the build that actually shipped.
    CHECK(pci::gdalDriverAvailable("GTiff"));

    CHECK_FALSE(pci::gdalDriverAvailable("NoSuchDriverExists"));
    CHECK_FALSE(pci::gdalDriverAvailable(""));
    const pci::GdalCatalogCapabilities &capabilities =
        pci::gdalCatalogCapabilities();

    // The test runtime must support catalogs as well as ordinary rasters.
    // Packaged applications have their own capability smoke check.
    CHECK(capabilities.tileIndex);
    CHECK(capabilities.virtualRaster);
    CHECK(capabilities.catalogImport());
    CHECK((capabilities.geoPackage || capabilities.flatGeobuf ||
           capabilities.shapefile));
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
    {
        struct RestoreCacheLimit final {
            std::uint64_t bytes;
            ~RestoreCacheLimit()
            {
                pci::setGdalBlockCacheBytes(bytes);
            }
        } restore{original};

        constexpr std::uint64_t requested = 64ULL * 1024ULL * 1024ULL;
        pci::setGdalBlockCacheBytes(requested);
        CHECK(pci::gdalBlockCacheBytes() == requested);
        CHECK(pci::gdalBlockCacheUsedBytes() <= pci::gdalBlockCacheBytes());
    }
    CHECK(pci::gdalBlockCacheBytes() == original);
}

TEST_CASE("GDAL error scopes isolate nested errors and restore capture",
          "[component][gdal]")
{
    const pci::GdalErrorScope outer;
    CHECK(outer.message().empty());
    CPLError(CE_Failure, CPLE_AppDefined, "outer before nesting");
    CHECK(outer.message() == "outer before nesting");
    {
        const pci::GdalErrorScope inner;
        CHECK(inner.message().empty());
        CPLError(CE_Failure, CPLE_AppDefined, "inner error");
        CHECK(inner.message() == "inner error");
        CHECK(outer.message() == "outer before nesting");
    }
    CHECK(outer.message() == "outer before nesting");
    CPLError(CE_Failure, CPLE_AppDefined, "outer after nesting");
    CHECK(outer.message() == "outer after nesting");
    CPLErrorReset();
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
