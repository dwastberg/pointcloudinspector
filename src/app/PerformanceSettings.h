#pragma once

#include <cstdint>

namespace pci {

inline constexpr std::uint64_t defaultCpuCacheMebibytes = 1024;
inline constexpr std::uint64_t defaultGpuCacheMebibytes = 512;
inline constexpr std::uint64_t defaultMaximumLoadPoints = 10'000'000;
inline constexpr int maximumCacheMebibytes = 2'097'152;
inline constexpr double maximumLoadPointsSetting = 1'000'000'000'000.0;

// Raster budgets are deliberately separate from the point caches: three
// different allocators hold raster pixels, and budgeting two while ignoring
// the third produces metrics that report compliance while process memory grows
// without bound.
inline constexpr std::uint64_t defaultRasterCpuCacheMebibytes = 256;
inline constexpr std::uint64_t defaultRasterGpuCacheMebibytes = 256;
inline constexpr std::uint64_t defaultGdalCacheMebibytes = 128;
inline constexpr std::uint32_t defaultRasterReadWorkers = 2;

// Minima below which the feature cannot function: the GPU floor is sixteen
// guttered RGBA tiles plus conservative binding accounting.
inline constexpr std::uint64_t minimumRasterCpuCacheMebibytes = 16;
inline constexpr std::uint64_t minimumRasterGpuCacheMebibytes = 5;
inline constexpr std::uint64_t minimumGdalCacheMebibytes = 1;
inline constexpr std::uint32_t minimumRasterReadWorkers = 1;
// Each additional handle to a VRT or GTI dataset opens its own member
// datasets, so the ceiling stays low on purpose.
inline constexpr std::uint32_t maximumRasterReadWorkers = 8;

struct RasterPerformanceSettings {
    std::uint64_t cpuCacheMebibytes = defaultRasterCpuCacheMebibytes;
    std::uint64_t gpuCacheMebibytes = defaultRasterGpuCacheMebibytes;
    // Process-global and invisible to the application's own accounting, which
    // is why it is set explicitly rather than left at GDAL's 5%-of-RAM default.
    std::uint64_t gdalCacheMebibytes = defaultGdalCacheMebibytes;
    // Persisted but applied at startup: the read pool is created once.
    std::uint32_t readWorkers = defaultRasterReadWorkers;

    bool operator==(const RasterPerformanceSettings &) const = default;
};

struct PerformanceSettings {
    bool automaticCpuCache = true;
    std::uint64_t cpuCacheMebibytes = defaultCpuCacheMebibytes;
    std::uint64_t gpuCacheMebibytes = defaultGpuCacheMebibytes;
    std::uint64_t maximumLoadPoints = defaultMaximumLoadPoints;
    RasterPerformanceSettings raster;

    bool operator==(const PerformanceSettings &) const = default;
};

// Clamps every raster budget into its supported range. Validation happens
// before any byte conversion, so a value that would overflow a multiplication
// is rejected rather than wrapped.
[[nodiscard]] RasterPerformanceSettings
clampRasterPerformanceSettings(RasterPerformanceSettings settings) noexcept;

[[nodiscard]] constexpr std::uint64_t
mebibytesToBytes(const std::uint64_t mebibytes) noexcept
{
    constexpr std::uint64_t bytesPerMebibyte = 1024ULL * 1024ULL;
    constexpr std::uint64_t ceiling =
        static_cast<std::uint64_t>(maximumCacheMebibytes);
    return (mebibytes > ceiling ? ceiling : mebibytes) * bytesPerMebibyte;
}

} // namespace pci
