#pragma once

#include <cstdint>

namespace pci {

inline constexpr std::uint64_t defaultCpuCacheMebibytes = 1024;
inline constexpr std::uint64_t defaultGpuCacheMebibytes = 512;
inline constexpr std::uint64_t defaultMaximumLoadPoints = 10'000'000;
inline constexpr int maximumCacheMebibytes = 2'097'152;
inline constexpr double maximumLoadPointsSetting = 1'000'000'000'000.0;

struct PerformanceSettings {
    bool automaticCpuCache = true;
    std::uint64_t cpuCacheMebibytes = defaultCpuCacheMebibytes;
    std::uint64_t gpuCacheMebibytes = defaultGpuCacheMebibytes;
    std::uint64_t maximumLoadPoints = defaultMaximumLoadPoints;

    bool operator==(const PerformanceSettings &) const = default;
};

} // namespace pci
