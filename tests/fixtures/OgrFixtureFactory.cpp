#include "fixtures/OgrFixtureFactory.h"
#include <pci/adapters/gdal/runtime/GdalRuntime.h>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <fstream>
#include <stdexcept>

namespace pci::test {
void writeOgrFixtureFeature(OGRLayer *layer, const OGRGeometry &geometry)
{
    if (!layer)
        throw std::runtime_error("could not create OGR fixture layer");
    const std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        feature{OGRFeature::CreateFeature(layer->GetLayerDefn()),
                &OGRFeature::DestroyFeature};
    if (!feature || feature->SetGeometry(&geometry) != OGRERR_NONE)
        throw std::runtime_error(
            "could not create OGR fixture feature geometry");
    if (layer->CreateFeature(feature.get()) != OGRERR_NONE)
        throw std::runtime_error("could not write OGR fixture feature");
}

namespace {

void addGpkgLayers(GDALDataset *dataset, OGRSpatialReference &srs)
{
    OGRLayer *controls =
        dataset->CreateLayer("controls", &srs, wkbPoint, nullptr);
    pci::test::writeOgrFixtureFeature(controls, OGRPoint(674000.0, 6580000.0));

    OGRLayer *roads =
        dataset->CreateLayer("roads", &srs, wkbCircularString, nullptr);
    OGRCircularString road;
    road.addPoint(674000.0, 6580000.0);
    road.addPoint(674050.0, 6580100.0);
    road.addPoint(674100.0, 6580000.0);
    pci::test::writeOgrFixtureFeature(roads, road);

    OGRLayer *parcels =
        dataset->CreateLayer("parcels", &srs, wkbPolygon, nullptr);
    OGRPolygon parcel;
    OGRLinearRing exterior;
    exterior.addPoint(674000.0, 6580000.0);
    exterior.addPoint(674100.0, 6580000.0);
    exterior.addPoint(674100.0, 6580100.0);
    exterior.addPoint(674000.0, 6580100.0);
    exterior.addPoint(674000.0, 6580000.0);
    if (parcel.addRing(&exterior) != OGRERR_NONE)
        throw std::runtime_error("could not add OGR fixture ring");
    OGRLinearRing hole;
    hole.addPoint(674020.0, 6580020.0);
    hole.addPoint(674020.0, 6580080.0);
    hole.addPoint(674080.0, 6580080.0);
    hole.addPoint(674080.0, 6580020.0);
    hole.addPoint(674020.0, 6580020.0);
    if (parcel.addRing(&hole) != OGRERR_NONE)
        throw std::runtime_error("could not add OGR fixture ring");
    pci::test::writeOgrFixtureFeature(parcels, parcel);

    OGRLayer *survey =
        dataset->CreateLayer("survey", &srs, wkbGeometryCollection25D, nullptr);
    OGRGeometryCollection collection;
    const OGRPoint point(674025.0, 6580025.0, 7.0);
    if (collection.addGeometry(&point) != OGRERR_NONE)
        throw std::runtime_error("could not add OGR fixture point");
    OGRLineString surveyLine;
    surveyLine.addPoint(674010.0, 6580010.0);
    surveyLine.addPoint(674090.0, 6580090.0);
    if (collection.addGeometry(&surveyLine) != OGRERR_NONE)
        throw std::runtime_error("could not add OGR fixture line");
    pci::test::writeOgrFixtureFeature(survey, collection);
}

} // namespace

OgrFixturePaths writeOgrFixtures(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    OgrFixturePaths paths{.geoJson = directory / "vector-fixture.geojson",
                          .geoPackage = directory / "vector-fixture.gpkg"};
    {
        std::ofstream output(paths.geoJson);
        if (!output)
            throw std::runtime_error("could not create GeoJSON fixture");
        output << R"({"type":"FeatureCollection","features":[
{"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]],[[2,2],[2,8],[8,8],[8,2],[2,2]]]}},
{"type":"Feature","geometry":{"type":"LineString","coordinates":[[20,0],[30,10]]}},
{"type":"Feature","geometry":{"type":"Point","coordinates":[40,5]}}]})";
    }
    GDALAllRegister();
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!driver)
        throw std::runtime_error("GPKG GDAL driver is unavailable");
    const GdalDatasetPtr dataset{driver->Create(
        paths.geoPackage.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr)};
    if (!dataset)
        throw std::runtime_error("could not create GPKG fixture");
    OGRSpatialReference srs;
    if (srs.importFromEPSG(3006) != OGRERR_NONE) {
        throw std::runtime_error("could not create EPSG:3006 fixture CRS");
    }
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    addGpkgLayers(dataset.get(), srs);
    return paths;
}

} // namespace pci::test
