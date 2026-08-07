#pragma once

#include <gdal_priv.h>

#include <filesystem>
#include <memory>

namespace pci {

struct GdalDatasetDeleter {
    void operator()(GDALDataset *dataset) const noexcept
    {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

using GdalDatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetDeleter>;

// Opens a dataset for read-only raster access, reporting the driver's own
// error text rather than a generic open failure. Throws RasterImportError.
[[nodiscard]] GdalDatasetPtr
openRasterDataset(const std::filesystem::path &path);

} // namespace pci
