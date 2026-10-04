#include "fixtures/OgrFixtureFactory.h"
#include <pci/adapters/gdal/runtime/GdalRuntime.h>
#include <pci/adapters/ogr/OgrVectorLoader.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

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
        : TemporaryGeoJson(R"(
 {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]],[[2,2],[2,8],[8,8],[8,2],[2,2]]]}},
 {"type":"Feature","geometry":{"type":"LineString","coordinates":[[20,0],[30,10]]}},
 {"type":"Feature","geometry":{"type":"Point","coordinates":[40,5]}}
)")
    {
    }

    explicit TemporaryGeoJson(const std::string_view features)
        : path_(temporaryPath(directory_) / "fixture.geojson")
    {
        std::ofstream output(path_);
        output << R"({"type":"FeatureCollection","features":[)" << features
               << "]}";
        output.close();
        if (!output)
            throw std::runtime_error("could not write GeoJSON fixture");
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
        if (srs_.importFromEPSG(3006) != OGRERR_NONE) {
            throw std::runtime_error("could not create EPSG:3006 fixture CRS");
        }
        srs_.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dataset_.reset(driver->Create(
            path_.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!dataset_)
            throw std::runtime_error("could not create GPKG fixture");
        addPointLayer();
        addLineLayer();
        addPolygonLayer();
        addMixedLayer();
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    void addPointLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("controls", &srs_, wkbPoint, nullptr);
        pci::test::writeOgrFixtureFeature(layer, OGRPoint(674000.0, 6580000.0));
    }

    void addLineLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("roads", &srs_, wkbLineString, nullptr);
        OGRLineString line;
        line.addPoint(674000.0, 6580000.0);
        line.addPoint(674050.0, 6580050.0);
        pci::test::writeOgrFixtureFeature(layer, line);
    }

    void addPolygonLayer()
    {
        OGRLayer *layer =
            dataset_->CreateLayer("parcels", &srs_, wkbPolygon, nullptr);
        OGRPolygon polygon;
        OGRLinearRing exterior;
        exterior.addPoint(674000.0, 6580000.0);
        exterior.addPoint(674100.0, 6580000.0);
        exterior.addPoint(674100.0, 6580100.0);
        exterior.addPoint(674000.0, 6580100.0);
        exterior.addPoint(674000.0, 6580000.0);
        if (polygon.addRing(&exterior) != OGRERR_NONE)
            throw std::runtime_error("could not add OGR fixture ring");
        OGRLinearRing hole;
        hole.addPoint(674020.0, 6580020.0);
        hole.addPoint(674020.0, 6580080.0);
        hole.addPoint(674080.0, 6580080.0);
        hole.addPoint(674080.0, 6580020.0);
        hole.addPoint(674020.0, 6580020.0);
        if (polygon.addRing(&hole) != OGRERR_NONE)
            throw std::runtime_error("could not add OGR fixture ring");
        pci::test::writeOgrFixtureFeature(layer, polygon);
    }

    void addMixedLayer()
    {
        OGRLayer *layer = dataset_->CreateLayer(
            "survey", &srs_, wkbGeometryCollection25D, nullptr);
        OGRGeometryCollection collection;
        const OGRPoint point(674025.0, 6580025.0, 7.0);
        if (collection.addGeometry(&point) != OGRERR_NONE)
            throw std::runtime_error("could not add OGR fixture point");
        OGRLineString line;
        line.addPoint(674010.0, 6580010.0);
        line.addPoint(674090.0, 6580090.0);
        if (collection.addGeometry(&line) != OGRERR_NONE)
            throw std::runtime_error("could not add OGR fixture line");
        pci::test::writeOgrFixtureFeature(layer, collection);
    }

    QTemporaryDir directory_;
    std::filesystem::path path_;
    OGRSpatialReference srs_;
    pci::GdalDatasetPtr dataset_;
};

TEST_CASE("OGR fixture feature failures unwind safely", "[component][ogr]")
{
    const OGRPoint point(674000.0, 6580000.0);
    REQUIRE_THROWS_WITH(pci::test::writeOgrFixtureFeature(nullptr, point),
                        "could not create OGR fixture layer");

    QTemporaryDir directory;
    const auto paths = pci::test::writeOgrFixtures(temporaryPath(directory));
    const pci::GdalDatasetPtr dataset{
        static_cast<GDALDataset *>(GDALOpenEx(paths.geoPackage.string().c_str(),
                                              GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                              nullptr,
                                              nullptr,
                                              nullptr))};
    REQUIRE(dataset);
    auto *layer = dataset->GetLayerByName("controls");
    REQUIRE(layer);
    REQUIRE_THROWS_WITH(pci::test::writeOgrFixtureFeature(layer, point),
                        "could not write OGR fixture feature");
    CHECK(layer->GetFeatureCount() == 1);
}

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

TEST_CASE("OGR uses the shared dataset owner with vector-only open flags",
          "[component][ogr][ownership]")
{
    TemporaryGeoJson fixture;
    pci::GdalDatasetOpenResult vector =
        pci::tryOpenGdalDataset(fixture.path(), pci::GdalDatasetKind::Vector);
    REQUIRE(vector.dataset);
    pci::GdalDatasetOpenResult raster =
        pci::tryOpenGdalDataset(fixture.path(), pci::GdalDatasetKind::Raster);
    CHECK_FALSE(raster.dataset);
}

TEST_CASE("OGR missing-source errors preserve their public context",
          "[component][ogr][ownership]")
{
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = "no-such-vector-file.gpkg";
    try {
        static_cast<void>(loader.inspect(request));
        FAIL("expected the missing vector source to fail");
    } catch (const pci::VectorImportError &error) {
        CHECK(
            std::string(error.what())
                .starts_with(
                    "Could not open vector source 'no-such-vector-file.gpkg'"));
    }
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
    const auto &parcelsWkt = preflight.sublayers.at(2).spatialReferenceWkt;
    CAPTURE(parcelsWkt);
    REQUIRE_FALSE(parcelsWkt.empty());
    OGRSpatialReference actualCrs;
    REQUIRE(actualCrs.importFromWkt(parcelsWkt.c_str()) == OGRERR_NONE);
    OGRSpatialReference expectedCrs;
    REQUIRE(expectedCrs.importFromEPSG(3006) == OGRERR_NONE);
    actualCrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    expectedCrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    CHECK(actualCrs.IsSame(&expectedCrs));

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

TEST_CASE("OGR curve import honours the configured angular tolerance",
          "[component][ogr]")
{
    QTemporaryDir directory;
    const pci::test::OgrFixturePaths paths =
        pci::test::writeOgrFixtures(temporaryPath(directory));
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = paths.geoPackage;
    request.origin = std::array<double, 2>{674000.0, 6580000.0};
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 4);
    request.limits.curveMaximumAngleStepDegrees = 4.0;
    const auto fine =
        loader.loadSublayer(request, preflight.sublayers.at(1).key);
    request.limits.curveMaximumAngleStepDegrees = 20.0;
    const auto coarse =
        loader.loadSublayer(request, preflight.sublayers.at(1).key);
    REQUIRE(fine);
    REQUIRE(coarse);
    REQUIRE_FALSE(coarse->segments.empty());
    REQUIRE(fine->segments.size() > coarse->segments.size());
    for (const auto &data : {fine, coarse}) {
        CHECK(data->segments.front().x0 + data->origin.x ==
              Catch::Approx(674000.0));
        CHECK(data->segments.front().y0 + data->origin.y ==
              Catch::Approx(6580000.0));
        CHECK(data->segments.back().x1 + data->origin.x ==
              Catch::Approx(674100.0));
        CHECK(data->segments.back().y1 + data->origin.y ==
              Catch::Approx(6580000.0));
    }
}

TEST_CASE("OGR origin probing skips null geometry and visits collections",
          "[component][ogr]")
{
    std::string features;
    SECTION("null geometry before a point")
    {
        features = R"({"type":"Feature","geometry":null},
{"type":"Feature","geometry":{"type":"Point","coordinates":[40,5]}})";
    }
    SECTION("point inside a collection")
    {
        features = R"({"type":"Feature","geometry":{"type":"GeometryCollection",
"geometries":[{"type":"Point","coordinates":[40,5]}]}})";
    }
    TemporaryGeoJson fixture(features);
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    const std::array keys{preflight.sublayers.front().key};
    CHECK(loader.probeOrigin(request, keys) == std::array<double, 2>{40, 5});
}

TEST_CASE("OGR origin probing rejects a source without usable coordinates",
          "[component][ogr]")
{
    TemporaryGeoJson fixture(R"({"type":"Feature","geometry":null})");
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    const std::array keys{preflight.sublayers.front().key};
    CHECK_THROWS_AS(loader.probeOrigin(request, keys), pci::VectorImportError);
}

TEST_CASE("OGR probing and loading honour pre-requested cancellation",
          "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    const std::array keys{preflight.sublayers.front().key};
    std::stop_source stop;
    stop.request_stop();
    request.stopToken = stop.get_token();
    CHECK_THROWS_AS(loader.probeOrigin(request, keys),
                    pci::VectorImportCancelled);
    CHECK_THROWS_AS(loader.loadSublayer(request, keys.front()),
                    pci::VectorImportCancelled);
}

TEST_CASE("OGR probing and loading reject unavailable sublayers",
          "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    for (const int index : {-1, 1}) {
        CAPTURE(index);
        const std::array keys{pci::VectorSublayerKey{.index = index}};
        CHECK_THROWS_AS(loader.probeOrigin(request, keys),
                        pci::VectorImportError);
        CHECK_THROWS_AS(loader.loadSublayer(request, keys.front()),
                        pci::VectorImportError);
    }
}

TEST_CASE("OGR source feature limits allow the boundary and reject excess",
          "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    request.limits.maximumSourceFeatures = 3;
    const auto data =
        loader.loadSublayer(request, preflight.sublayers.front().key);
    REQUIRE(data);
    CHECK(data->featureCount == 3);
    request.limits.maximumSourceFeatures = 2;
    CHECK_THROWS_MATCHES(
        loader.loadSublayer(request, preflight.sublayers.front().key),
        pci::VectorImportLimitExceeded,
        Catch::Matchers::MessageMatches(
            Catch::Matchers::ContainsSubstring("maximumSourceFeatures")));
}

TEST_CASE("OGR curve limits reject excessive input or linearized geometry",
          "[component][ogr]")
{
    QTemporaryDir directory;
    const auto paths = pci::test::writeOgrFixtures(temporaryPath(directory));
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = paths.geoPackage;
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 4);
    const auto key = preflight.sublayers.at(1).key;
    std::string limitName;
    SECTION("control points")
    {
        request.limits.maximumCurveControlPoints = 2;
        limitName = "maximumCurveControlPoints";
    }
    SECTION("linearized points")
    {
        request.limits.maximumLinearizedCurvePoints = 3;
        limitName = "maximumLinearizedCurvePoints";
    }
    CHECK_THROWS_MATCHES(loader.loadSublayer(request, key),
                         pci::VectorImportLimitExceeded,
                         Catch::Matchers::MessageMatches(
                             Catch::Matchers::ContainsSubstring(limitName)));
}

TEST_CASE("OGR final progress counts source features", "[component][ogr]")
{
    TemporaryGeoJson fixture;
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    std::vector<pci::VectorImportProgress> progress;
    request.progress = [&progress](const auto value) {
        progress.push_back(value);
    };
    const auto data =
        loader.loadSublayer(request, preflight.sublayers.front().key);
    REQUIRE(data);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back().processed == 3);
    CHECK(progress.back().total == 3);
    CHECK(data->featureCount == progress.back().processed);
}

TEST_CASE("OGR intermediate progress supports cancellation without completion",
          "[component][ogr]")
{
    constexpr std::uint64_t featureCount = 8193;
    std::string features;
    for (std::uint64_t index = 0; index < featureCount; ++index) {
        if (index != 0)
            features += ',';
        features +=
            R"({"type":"Feature","geometry":{"type":"Point","coordinates":[1,2]}})";
    }
    TemporaryGeoJson fixture(features);
    pci::OgrVectorLoader loader;
    pci::VectorImportRequest request;
    request.sourcePath = fixture.path();
    const auto preflight = loader.inspect(request);
    REQUIRE(preflight.sublayers.size() == 1);
    const auto key = preflight.sublayers.front().key;
    std::vector<pci::VectorImportProgress> progress;
    std::stop_source stop;
    request.stopToken = stop.get_token();
    bool cancel = false;
    request.progress = [&](const pci::VectorImportProgress value) {
        progress.push_back(value);
        if (cancel && value.total == 0)
            stop.request_stop();
    };
    SECTION("successful import reports intermediate and final progress")
    {
        const auto data = loader.loadSublayer(request, key);
        REQUIRE(data);
        CHECK(data->featureCount == featureCount);
        REQUIRE(progress.size() >= 2);
        CHECK(progress.front().total == 0);
        CHECK(progress.back().processed == featureCount);
        CHECK(progress.back().total == featureCount);
    }
    SECTION("cancellation from intermediate progress prevents completion")
    {
        cancel = true;
        CHECK_THROWS_AS(loader.loadSublayer(request, key),
                        pci::VectorImportCancelled);
        REQUIRE_FALSE(progress.empty());
        CHECK(stop.stop_requested());
        for (const auto value : progress) {
            CHECK(value.total == 0);
            CHECK(value.processed < featureCount);
        }
    }
    std::uint64_t previous = 0;
    for (const auto value : progress) {
        CHECK(value.processed > previous);
        CHECK(value.processed <= featureCount);
        previous = value.processed;
    }
}

} // namespace
