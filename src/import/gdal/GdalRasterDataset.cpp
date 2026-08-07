#include "import/gdal/GdalRasterDataset.h"

#include "import/gdal/GdalRuntime.h"
#include "raster/RasterLayer.h"

#include <string>

namespace pci {

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
    if (dataset == nullptr) {
        const std::string detail = errors.message();
        throw RasterImportError(
            "Could not open raster source '" + path.string() + "'" +
            (detail.empty() ? std::string() : ": " + detail));
    }
    return GdalDatasetPtr{dataset};
}

} // namespace pci
