#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

class GDALDataset;

namespace pci {

struct GdalDatasetDeleter {
    void operator()(GDALDataset *dataset) const noexcept;
};

using GdalDatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetDeleter>;

enum class GdalDatasetKind : std::uint8_t {
    Raster,
    Vector,
};

struct GdalDatasetOpenResult {
    GdalDatasetPtr dataset;
    std::string error;
};

// Registers every GDAL and OGR driver exactly once per process. The raster and
// OGR adapters both call it; neither of them owns registration.
void ensureGdalRegistered();

// Captures GDAL's error text for the lifetime of the scope on the calling
// thread, so a failed call can report the driver's own message rather than a
// generic open failure.
class GdalErrorScope final {
public:
    GdalErrorScope();
    ~GdalErrorScope();
    GdalErrorScope(const GdalErrorScope &) = delete;
    GdalErrorScope &operator=(const GdalErrorScope &) = delete;
    GdalErrorScope(GdalErrorScope &&) = delete;
    GdalErrorScope &operator=(GdalErrorScope &&) = delete;

    [[nodiscard]] std::string message() const;

private:
    std::string message_;
    std::string *previous_ = nullptr;
};

// Opens one independently owned read-only handle. Raster and vector flags are
// deliberately distinct; callers retain their adapter-specific driver and
// error policy instead of hiding it in a universal loader.
[[nodiscard]] GdalDatasetOpenResult
tryOpenGdalDataset(const std::filesystem::path &path, GdalDatasetKind kind);

[[nodiscard]] std::string
gdalOpenFailureMessage(std::string_view sourceKind,
                       const std::filesystem::path &path,
                       std::string_view detail = {});

struct GdalRuntimeVersion {
    int major = 0;
    int minor = 0;
    int revision = 0;
    std::string release;
};

[[nodiscard]] GdalRuntimeVersion gdalRuntimeVersion();

// A version floor is not a driver probe. Optional index and storage drivers
// can be absent from a newer build, so capability is measured rather than
// inferred from the version.
[[nodiscard]] bool gdalDriverAvailable(std::string_view driverName);

// What this build can actually open, probed once. A user's bug report needs to
// distinguish "GTI missing" from "GTI broken", which a version number alone
// cannot do.
//
// Index drivers are listed separately from GTI because a catalog names its
// index in its own file: GTI can be present while the format holding the index
// is not, and that failure must name the missing index driver rather than look
// like a broken catalog.
struct GdalCatalogCapabilities {
    GdalRuntimeVersion version;
    bool tileIndex = false;     // GTI
    bool virtualRaster = false; // VRT
    bool geoPackage = false;    // GPKG
    bool flatGeobuf = false;    // FlatGeobuf
    bool shapefile = false;     // ESRI Shapefile

    // Ordinary GeoTIFF and VRT import stays available without GTI; only
    // catalog import depends on it.
    [[nodiscard]] bool catalogImport() const noexcept
    {
        return tileIndex && (geoPackage || flatGeobuf || shapefile);
    }
};

[[nodiscard]] const GdalCatalogCapabilities &gdalCatalogCapabilities();

// GDAL's block cache defaults to 5% of physical RAM, is process-global, and is
// invisible to the application's own byte accounting. Setting it explicitly is
// what makes the documented memory envelope true rather than aspirational.
void setGdalBlockCacheBytes(std::uint64_t bytes);
[[nodiscard]] std::uint64_t gdalBlockCacheBytes();
[[nodiscard]] std::uint64_t gdalBlockCacheUsedBytes();

} // namespace pci
