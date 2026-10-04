#pragma once

#include <pci/desktop/viewport/RenderMetrics.h>

#include <pci/rendering/RenderTelemetry.h>

namespace pci {

[[nodiscard]] RenderMetrics
projectRenderMetrics(const RenderTelemetrySnapshot &telemetry);

} // namespace pci
