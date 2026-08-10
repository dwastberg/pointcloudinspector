#include "import/gdal/GdalRuntime.h"

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <gdal_version.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

// The supported API baseline. GTI, which the large-catalog requirement needs,
// was introduced in 3.9, so an older build cannot satisfy that requirement at
// all. The runtime driver probes below remain necessary regardless.
#if GDAL_VERSION_NUM < 3090000
#error "Point Cloud Inspector requires GDAL 3.9 or newer"
#endif

namespace pci {
namespace {

thread_local std::string *activeError = nullptr;

void captureError(const CPLErr, const int, const char *message)
{
    if (activeError != nullptr && message != nullptr) {
        *activeError = message;
    }
}

[[nodiscard]] std::uint64_t nonNegative(const GIntBig value) noexcept
{
    return value <= 0 ? 0U : static_cast<std::uint64_t>(value);
}

} // namespace

void ensureGdalRegistered()
{
    static const bool registered = [] {
        GDALAllRegister();
        return true;
    }();
    static_cast<void>(registered);
}

GdalErrorScope::GdalErrorScope()
{
    activeError = &message_;
    CPLPushErrorHandler(captureError);
}

GdalErrorScope::~GdalErrorScope()
{
    CPLPopErrorHandler();
    activeError = nullptr;
}

std::string GdalErrorScope::message() const
{
    return message_;
}

GdalRuntimeVersion gdalRuntimeVersion()
{
    ensureGdalRegistered();

    GdalRuntimeVersion version;
    if (const char *release = GDALVersionInfo("RELEASE_NAME");
        release != nullptr) {
        version.release = release;
    }
    const char *numeric = GDALVersionInfo("VERSION_NUM");
    const long packed =
        numeric == nullptr ? 0L : std::strtol(numeric, nullptr, 10);
    version.major = static_cast<int>(packed / 1000000L);
    version.minor = static_cast<int>((packed / 10000L) % 100L);
    version.revision = static_cast<int>((packed / 100L) % 100L);
    return version;
}

bool gdalDriverAvailable(const std::string_view driverName)
{
    ensureGdalRegistered();
    const std::string name(driverName);
    return GDALGetDriverByName(name.c_str()) != nullptr;
}

const GdalCatalogCapabilities &gdalCatalogCapabilities()
{
    // Probed once: driver registration cannot change after startup, and a
    // per-import probe would put a lookup on the import path for an answer
    // that never varies.
    static const GdalCatalogCapabilities capabilities = [] {
        ensureGdalRegistered();
        return GdalCatalogCapabilities{
            .version = gdalRuntimeVersion(),
            .tileIndex = gdalDriverAvailable("GTI"),
            .virtualRaster = gdalDriverAvailable("VRT"),
            .geoPackage = gdalDriverAvailable("GPKG"),
            .flatGeobuf = gdalDriverAvailable("FlatGeobuf"),
            .shapefile = gdalDriverAvailable("ESRI Shapefile"),
        };
    }();
    return capabilities;
}

void setGdalBlockCacheBytes(const std::uint64_t bytes)
{
    ensureGdalRegistered();
    constexpr auto representable =
        static_cast<std::uint64_t>(std::numeric_limits<GIntBig>::max());
    GDALSetCacheMax64(static_cast<GIntBig>(std::min(bytes, representable)));
}

std::uint64_t gdalBlockCacheBytes()
{
    ensureGdalRegistered();
    return nonNegative(GDALGetCacheMax64());
}

std::uint64_t gdalBlockCacheUsedBytes()
{
    ensureGdalRegistered();
    return nonNegative(GDALGetCacheUsed64());
}

} // namespace pci
