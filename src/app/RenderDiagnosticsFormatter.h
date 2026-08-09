#pragma once

#include "import/PointCloudLoadController.h"
#include "renderer/RenderMetrics.h"

#include <QString>

#include <optional>

namespace pci {

class RenderDiagnosticsFormatter final {
public:
    // gdalCacheUsedBytes is the third allocator holding raster pixels. It is
    // passed in rather than read here because the UI target does not link
    // GDAL; absent, the raster line reports the two budgets it can see and
    // marks the third unavailable rather than implying it is zero.
    [[nodiscard]] static QString
    panelText(const RenderMetrics &metrics,
              const PointCloudLoadControllerMetrics &loadMetrics,
              std::optional<std::uint64_t> gdalCacheUsedBytes = std::nullopt);
    [[nodiscard]] static QString
    statusText(const RenderMetrics &metrics,
               std::optional<double> timeToFirstPointsMilliseconds);
};

} // namespace pci
