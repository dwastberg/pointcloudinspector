#include "import/SupportedSource.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

TEST_CASE("supported source classification applies compound-name precedence",
          "[unit][source-open]")
{
    using pci::SupportedSourceKind;
    CHECK(pci::supportedSourceKind("cloud.LAS") ==
          SupportedSourceKind::PointCloud);
    CHECK(pci::supportedSourceKind("cloud.COPC.LAZ") ==
          SupportedSourceKind::PointCloud);
    CHECK(pci::supportedSourceKind("dataset/ept.json") ==
          SupportedSourceKind::PointCloud);
    CHECK(pci::supportedSourceKind("ordinary.json") ==
          SupportedSourceKind::Vector);
    CHECK(pci::supportedSourceKind("tiles.gti.fgb") ==
          SupportedSourceKind::Raster);
    CHECK(pci::supportedSourceKind("tiles.gti.shp") ==
          SupportedSourceKind::Raster);
}

TEST_CASE("GeoPackage sources are vector-only in unified opening",
          "[unit][source-open][gpkg]")
{
    using pci::SupportedSourceKind;
    CHECK(pci::supportedSourceKind("survey.gpkg") ==
          SupportedSourceKind::Vector);
    CHECK(pci::supportedSourceKind("catalog.gti.gpkg") ==
          SupportedSourceKind::Vector);
}

TEST_CASE("source classification preserves supported and unsupported order",
          "[unit][source-open]")
{
    pci::SourceClassification result = pci::classifySupportedSources(
        {"first.laz", "notes.txt", "ortho.tif", "roads.gpkg"});
    REQUIRE(result.supported.size() == 3);
    CHECK(result.supported[0].path == std::filesystem::path("first.laz"));
    CHECK(result.supported[1].path == std::filesystem::path("ortho.tif"));
    CHECK(result.supported[2].path == std::filesystem::path("roads.gpkg"));
    REQUIRE(result.unsupported.size() == 1);
    CHECK(result.unsupported.front() == std::filesystem::path("notes.txt"));
}
