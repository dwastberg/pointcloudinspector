#include "app/PerformanceSettings.h"

#include <algorithm>

namespace pci {

RasterPerformanceSettings
clampRasterPerformanceSettings(RasterPerformanceSettings settings) noexcept
{
    const auto ceiling = static_cast<std::uint64_t>(maximumCacheMebibytes);
    settings.cpuCacheMebibytes = std::clamp(
        settings.cpuCacheMebibytes, minimumRasterCpuCacheMebibytes, ceiling);
    settings.gpuCacheMebibytes = std::clamp(
        settings.gpuCacheMebibytes, minimumRasterGpuCacheMebibytes, ceiling);
    settings.gdalCacheMebibytes = std::clamp(
        settings.gdalCacheMebibytes, minimumGdalCacheMebibytes, ceiling);
    settings.readWorkers = std::clamp(settings.readWorkers,
                                      minimumRasterReadWorkers,
                                      maximumRasterReadWorkers);
    return settings;
}

} // namespace pci
