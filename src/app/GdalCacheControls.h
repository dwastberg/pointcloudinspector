#pragma once

#include <cstdint>
#include <functional>

namespace pci {

// GDAL's block cache is process-global, is the third allocator holding raster
// pixels, and lives behind the GDAL adapter boundary. The UI target must not
// link GDAL, so it reaches the cache through hooks the application layer
// installs at bootstrap instead.
//
// Both hooks are optional. When they are absent the settings still persist and
// the diagnostics simply omit the figure, which is what happens in the widget
// tests that construct a window without a GDAL-backed application.
struct GdalCacheControls {
    std::function<void(std::uint64_t)> setByteBudget;
    std::function<std::uint64_t()> usedBytes;

    [[nodiscard]] bool valid() const noexcept
    {
        return static_cast<bool>(setByteBudget) && static_cast<bool>(usedBytes);
    }
};

} // namespace pci
