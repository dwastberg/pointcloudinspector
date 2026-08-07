#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pci {

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
};

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

// GDAL's block cache defaults to 5% of physical RAM, is process-global, and is
// invisible to the application's own byte accounting. Setting it explicitly is
// what makes the documented memory envelope true rather than aspirational.
void setGdalBlockCacheBytes(std::uint64_t bytes);
[[nodiscard]] std::uint64_t gdalBlockCacheBytes();
[[nodiscard]] std::uint64_t gdalBlockCacheUsedBytes();

} // namespace pci
