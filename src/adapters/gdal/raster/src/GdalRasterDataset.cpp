#include <pci/adapters/gdal/GdalRasterDataset.h>

#include <pci/adapters/gdal/runtime/GdalRuntime.h>
#include <pci/raster/RasterLayer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace pci {
namespace {

[[nodiscard]] std::string lowercased(const std::string &text)
{
    std::string result = text;
    std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

[[nodiscard]] bool endsWith(const std::string &text,
                            const std::string_view tail)
{
    return text.size() >= tail.size() &&
           std::string_view(text).substr(text.size() - tail.size()) == tail;
}

} // namespace

RasterSourceDrivers rasterSourceDriversFor(const std::filesystem::path &path)
{
    const std::string name = lowercased(path.filename().string());
    if (endsWith(name, ".vrt")) {
        return {.container = "VRT", .index = {}};
    }
    // A tile index names its own storage format in its extension, so the
    // required index driver is known without reading the file.
    if (endsWith(name, ".gti.gpkg")) {
        return {.container = "GTI", .index = "GPKG"};
    }
    if (endsWith(name, ".gti.fgb")) {
        return {.container = "GTI", .index = "FlatGeobuf"};
    }
    if (endsWith(name, ".gti.shp")) {
        return {.container = "GTI", .index = "ESRI Shapefile"};
    }
    if (endsWith(name, ".gti")) {
        return {.container = "GTI", .index = {}};
    }
    return {};
}

std::string_view
missingRasterDriver(const RasterSourceDrivers required,
                    const GdalCatalogCapabilities &capabilities) noexcept
{
    const auto present = [&capabilities](const std::string_view driver) {
        if (driver == "GTI") {
            return capabilities.tileIndex;
        }
        if (driver == "VRT") {
            return capabilities.virtualRaster;
        }
        if (driver == "GPKG") {
            return capabilities.geoPackage;
        }
        if (driver == "FlatGeobuf") {
            return capabilities.flatGeobuf;
        }
        if (driver == "ESRI Shapefile") {
            return capabilities.shapefile;
        }
        // An unrecognised name is not evidence of absence.
        return true;
    };
    // The container is reported first: without it the index format is moot.
    for (const std::string_view driver : {required.container, required.index}) {
        if (!driver.empty() && !present(driver)) {
            return driver;
        }
    }
    return {};
}

GdalDatasetPtr openRasterDataset(const std::filesystem::path &path)
{
    GdalDatasetOpenResult opened =
        tryOpenGdalDataset(path, GdalDatasetKind::Raster);
    if (opened.dataset) {
        return std::move(opened.dataset);
    }

    // A stripped build fails to open a catalog with a message about the file,
    // which sends the user looking at their data. Naming the absent driver
    // sends them to their installation instead.
    const std::string_view missing = missingRasterDriver(
        rasterSourceDriversFor(path), gdalCatalogCapabilities());
    if (!missing.empty()) {
        throw RasterImportError(gdalOpenFailureMessage(
            "raster",
            path,
            "this GDAL build has no " + std::string(missing) +
                " driver. Reinstall or rebuild GDAL with it enabled."));
    }

    throw RasterImportError(
        gdalOpenFailureMessage("raster", path, opened.error));
}

} // namespace pci
