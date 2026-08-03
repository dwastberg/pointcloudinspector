#pragma once

#include "import/PointCloudLoadController.h"
#include "renderer/RenderMetrics.h"

#include <QString>

#include <optional>

namespace pci {

class RenderDiagnosticsFormatter final {
public:
    [[nodiscard]] static QString
    panelText(const RenderMetrics &metrics,
              const PointCloudLoadControllerMetrics &loadMetrics);
    [[nodiscard]] static QString
    statusText(const RenderMetrics &metrics,
               std::optional<double> timeToFirstPointsMilliseconds);
};

} // namespace pci
