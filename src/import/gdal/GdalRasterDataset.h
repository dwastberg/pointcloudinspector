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

// The drivers a path's own name implies. Derived from the filename alone: a
// catalog's members are never enumerated, opened, or counted here, which is
// what keeps import cost independent of catalog cardinality.
//
// Empty names mean "no specific driver is implied", in which case a failure is
// reported with the driver's own message and nothing more.
struct RasterSourceDrivers {
    std::string_view container; // GTI or VRT
    std::string_view index;     // the format holding a catalog's tile index
};

[[nodiscard]] RasterSourceDrivers
rasterSourceDriversFor(const std::filesystem::path &path);

// Opens a dataset for read-only raster access, reporting the driver's own
// error text rather than a generic open failure. When the path implies a
// driver this build does not have, the error names that driver instead.
// Throws RasterImportError.
[[nodiscard]] GdalDatasetPtr
openRasterDataset(const std::filesystem::path &path);

} // namespace pci
