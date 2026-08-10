#include "import/gdal/GdalRasterDataset.h"

#include "import/gdal/GdalRuntime.h"
#include "raster/RasterLayer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

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

GdalDatasetPtr openRasterDataset(const std::filesystem::path &path)
{
    ensureGdalRegistered();
    GdalErrorScope errors;
    auto *dataset = static_cast<GDALDataset *>(
        GDALOpenEx(path.string().c_str(),
                   GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
                   nullptr,
                   nullptr,
                   nullptr));
    if (dataset != nullptr) {
        return GdalDatasetPtr{dataset};
    }

    // A stripped build fails to open a catalog with a message about the file,
    // which sends the user looking at their data. Naming the absent driver
    // sends them to their installation instead.
    const RasterSourceDrivers required = rasterSourceDriversFor(path);
    for (const std::string_view driver : {required.container, required.index}) {
        if (driver.empty() || gdalDriverAvailable(driver)) {
            continue;
        }
        throw RasterImportError(
            "Could not open raster source '" + path.string() +
            "': this GDAL build has no " + std::string(driver) +
            " driver. Reinstall or rebuild GDAL with it enabled.");
    }

    const std::string detail = errors.message();
    throw RasterImportError("Could not open raster source '" + path.string() +
                            "'" +
                            (detail.empty() ? std::string() : ": " + detail));
}

} // namespace pci
