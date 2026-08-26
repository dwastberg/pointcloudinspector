#include "fixtures/OgrFixtureFactory.h"
#include "import/ogr/OgrVectorLoader.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

[[nodiscard]] std::filesystem::path temporaryPath(QTemporaryDir &directory)
{
    if (!directory.isValid()) {
        throw std::runtime_error("could not create private OGR fixture");
    }
    return QDir(directory.path()).filesystemPath();
}

class TemporaryGeoJson final {
public:
    TemporaryGeoJson()
        : path_(temporaryPath(directory_) / "fixture.geojson")
    {
        std::ofstream output(path_);
        output << R"({
"type":"FeatureCollection",
"features":[
 {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]],[[2,2],[2,8],[8,8],[8,2],[2,2]]]}},
 {"type":"Feature","geometry":{"type":"LineString","coordinates":[[20,0],[30,10]]}},
 {"type":"Feature","geometry":{"type":"Point","coordinates":[40,5]}}
]})";
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    QTemporaryDir directory_;
    std::filesystem::path path_;
};

class TemporaryGeoPackage final {
public:
    TemporaryGeoPackage()
        : path_(temporaryPath(directory_) / "fixture.gpkg")
    {
        GDALAllRegister();
        GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (!driver)
            throw std::runtime_error("GPKG GDAL driver is unavailable");
        dataset_ = driver->Create(
            path_.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!dataset_)
            throw std::runtime_error("could not create GPKG fixture");
        srs_.importFromEPSG(3006);
        srs_.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        addPointLayer();
        addLineLayer();
        addPolygonLayer();
        addMixedLayer();
    }

    ~TemporaryGeoPackage()
    {
        if (dataset_)
            GDALClose(dataset_);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    void addFeature(OGRLayer *layer, OGRGeometry *geometry)
    {
        OGRFeature *feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetGeometryDirectly(geometry);
        if (layer->CreateFeature(feature) != OGRERR_NONE) {
            OGRFeature::DestroyFeature(feature);
            throw std::runtime_error("could not write GPKG fixture feature");
        }
        OGRFeature::DestroyFeature(feature);
    }

    void addPointLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("controls", &srs_, wkbPoint, nullptr);
        addFeature(layer, new OGRPoint(674000.0, 6580000.0));
    }

    void addLineLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("roads", &srs_, wkbLineString, nullptr);
        auto *line = new OGRLineString();
        line->addPoint(674000.0, 6580000.0);
        line->addPoint(674050.0, 6580050.0);
        addFeature(layer, line);
    }

    void addPolygonLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("parcels", &srs_, wkbPolygon, nullptr);
        auto *polygon = new OGRPolygon();
        OGRLinearRing exterior;
        exterior.addPoint(674000.0, 6580000.0);
        exterior.addPoint(674100.0, 6580000.0);
        exterior.addPoint(674100.0, 6580100.0);
        exterior.addPoint(674000.0, 6580100.0);
        exterior.addPoint(674000.0, 6580000.0);
        polygon->addRing(&exterior);
        OGRLinearRing hole;
        hole.addPoint(674020.0, 6580020.0);
        hole.addPoint(674020.0, 6580080.0);
        hole.addPoint(674080.0, 6580080.0);
        hole.addPoint(674080.0, 6580020.0);
        hole.addPoint(674020.0, 6580020.0);
        polygon->addRing(&hole);
        addFeature(layer, polygon);
    }

    void addMixedLayer()
    {
        OGRLayer *layer = dataset_->CreateLayer(
            "survey", &srs_, wkbGeometryCollection, nullptr);
        auto *collection = new OGRGeometryCollection();
        collection->addGeometryDirectly(new OGRPoint(674025.0, 6580025.0, 7.0));
        auto *line = new OGRLineString();
        line->addPoint(674010.0, 6580010.0);
        line->addPoint(674090.0, 6580090.0);
        collection->addGeometryDirectly(line);
        addFeature(layer, collection);
    }

    QTemporaryDir directory_;
    std::filesystem::path path_;
    GDALDataset *dataset_ = nullptr;
    OGRSpatialReference srs_;
};

TEST_CASE("OGR vector loader inspects and loads a local vector file",
          "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    request.origin = std::array<double, 2>{0.0, 0.0};

    const pci::VectorImportPreflight preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    CHECK(preflight.sublayers.front().key.index == 0);

    const pci::VectorLayerDataPtr data =
        loader.loadSublayer(request, preflight.sublayers.front().key);
    REQUIRE(data);
    CHECK(data->summary.polygonParts == 1);
    CHECK(data->summary.lineParts == 1);
    CHECK(data->summary.pointParts == 1);
    CHECK(data->triangleCount() > 0);
    CHECK(data->segments.size() == 9);
    CHECK(data->markers.size() == 1);
}

TEST_CASE("OGR vector loader reports XY-disjoint extents", "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    request.origin = std::array<double, 2>{0.0, 0.0};
    request.targetExtent = {
        .minimum = {1'000.0, 1'000.0, -1.0},
        .maximum = {1'001.0, 1'001.0, 1.0},
    };
    const pci::VectorImportPreflight preflight = loader.inspect(request);
    const pci::VectorLayerDataPtr data =
        loader.loadSublayer(request, preflight.sublayers.front().key);
    REQUIRE(data);
    CHECK(data->extentDisjointXY);
}

TEST_CASE("OGR XY-disjoint detection ignores non-overlapping Z ranges",
          "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    request.origin = std::array<double, 2>{0.0, 0.0};
    request.targetExtent = {
        .minimum = {0.0, 0.0, 1'000.0},
        .maximum = {10.0, 10.0, 1'001.0},
    };
    const auto preflight = loader.inspect(request);
    const auto data =
        loader.loadSublayer(request, preflight.sublayers.front().key);
    REQUIRE(data);
    CHECK_FALSE(data->extentDisjointXY);
}

TEST_CASE("OGR GPKG fixture preserves projected CRS and multiple sublayers",
          "[component][ogr]")
{
    TemporaryGeoPackage fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    request.origin = std::array<double, 2>{674000.0, 6580000.0};
    const pci::VectorImportPreflight preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 4);
    CHECK(preflight.sublayers.at(0).key.name == "controls");
    CHECK(preflight.sublayers.at(1).key.name == "roads");
    CHECK(preflight.sublayers.at(2).key.name == "parcels");
    CHECK_FALSE(preflight.sublayers.at(2).spatialReferenceWkt.empty());
    CHECK(preflight.sublayers.at(2).spatialReferenceWkt.find("SWEREF99") !=
          std::string::npos);

    const auto parcels =
        loader.loadSublayer(request, preflight.sublayers.at(2).key);
    REQUIRE(parcels);
    CHECK(parcels->summary.polygonParts == 1);
    CHECK(parcels->triangleCount() > 0);
    const auto mixed =
        loader.loadSublayer(request, preflight.sublayers.at(3).key);
    REQUIRE(mixed);
    CHECK(mixed->summary.pointParts == 1);
    CHECK(mixed->summary.lineParts == 1);
    CHECK(mixed->summary.sourceHadZ);

    OGRSpatialReference wgs84;
    REQUIRE(wgs84.importFromEPSG(4326) == OGRERR_NONE);
    char *wgs84Wkt = nullptr;
    REQUIRE(wgs84.exportToWkt(&wgs84Wkt) == OGRERR_NONE);
    request.targetSpatialReferenceWkt = wgs84Wkt;
    CPLFree(wgs84Wkt);
    const auto mismatched =
        loader.loadSublayer(request, preflight.sublayers.at(0).key);
    REQUIRE(mismatched);
    CHECK(mismatched->crsMismatch);
}

TEST_CASE(
    "reusable OGR fixture factory writes the projected multi-layer corpus",
    "[component][ogr][fixture]")
{
    QTemporaryDir directory;
    const pci::test::OgrFixturePaths paths =
        pci::test::writeOgrFixtures(temporaryPath(directory));
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = paths.geoPackage;
    const auto preflight = loader.inspect(request);
    CHECK(std::filesystem::exists(paths.geoJson));
    CHECK(std::filesystem::exists(paths.geoPackage));
    CHECK(preflight.sublayers.size() == 4);
    const auto roads =
        loader.loadSublayer(request, preflight.sublayers.at(1).key);
    REQUIRE(roads);
    // The fixture's CircularString must be linearised using the configured
    // 4-degree maximum angle step, not passed through as its 3 controls.
    CHECK(roads->segments.size() > 8);
}

} // namespace
