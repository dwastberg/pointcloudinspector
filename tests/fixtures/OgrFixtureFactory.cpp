#include "fixtures/OgrFixtureFactory.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <fstream>
#include <stdexcept>

namespace pci::test {
namespace {

void addFeature(OGRLayer *layer, OGRGeometry *geometry)
{
    if (!layer)
        throw std::runtime_error("could not create OGR fixture layer");
    OGRFeature *feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feature->SetGeometryDirectly(geometry);
    if (layer->CreateFeature(feature) != OGRERR_NONE) {
        OGRFeature::DestroyFeature(feature);
        throw std::runtime_error("could not write OGR fixture feature");
    }
    OGRFeature::DestroyFeature(feature);
}

void addGpkgLayers(GDALDataset *dataset, OGRSpatialReference &srs)
{
    OGRLayer *controls =
        dataset->CreateLayer("controls", &srs, wkbPoint, nullptr);
    addFeature(controls, new OGRPoint(674000.0, 6580000.0));

    OGRLayer *roads =
        dataset->CreateLayer("roads", &srs, wkbCircularString, nullptr);
    auto *road = new OGRCircularString();
    road->addPoint(674000.0, 6580000.0);
    road->addPoint(674050.0, 6580100.0);
    road->addPoint(674100.0, 6580000.0);
    addFeature(roads, road);

    OGRLayer *parcels =
        dataset->CreateLayer("parcels", &srs, wkbPolygon, nullptr);
    auto *parcel = new OGRPolygon();
    OGRLinearRing exterior;
    exterior.addPoint(674000.0, 6580000.0);
    exterior.addPoint(674100.0, 6580000.0);
    exterior.addPoint(674100.0, 6580100.0);
    exterior.addPoint(674000.0, 6580100.0);
    exterior.addPoint(674000.0, 6580000.0);
    parcel->addRing(&exterior);
    OGRLinearRing hole;
    hole.addPoint(674020.0, 6580020.0);
    hole.addPoint(674020.0, 6580080.0);
    hole.addPoint(674080.0, 6580080.0);
    hole.addPoint(674080.0, 6580020.0);
    hole.addPoint(674020.0, 6580020.0);
    parcel->addRing(&hole);
    addFeature(parcels, parcel);

    OGRLayer *survey =
        dataset->CreateLayer("survey", &srs, wkbGeometryCollection, nullptr);
    auto *collection = new OGRGeometryCollection();
    collection->addGeometryDirectly(new OGRPoint(674025.0, 6580025.0, 7.0));
    auto *surveyLine = new OGRLineString();
    surveyLine->addPoint(674010.0, 6580010.0);
    surveyLine->addPoint(674090.0, 6580090.0);
    collection->addGeometryDirectly(surveyLine);
    addFeature(survey, collection);
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
    GDALDataset *dataset = driver->Create(
        paths.geoPackage.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset)
        throw std::runtime_error("could not create GPKG fixture");
    try {
        OGRSpatialReference srs;
        if (srs.importFromEPSG(3006) != OGRERR_NONE) {
            throw std::runtime_error("could not create EPSG:3006 fixture CRS");
        }
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        addGpkgLayers(dataset, srs);
    } catch (...) {
        GDALClose(dataset);
        throw;
    }
    GDALClose(dataset);
    return paths;
}

} // namespace pci::test
