#pragma once

#include <pci/desktop/viewport/GraphicsApi.h>

#include <QRhiWidget>
#include <rhi/qrhi.h>

#include <optional>

namespace pci {

// Auto deliberately has no QRhiWidget value: callers must leave the widget's
// API untouched so Qt can choose its platform default.
[[nodiscard]] std::optional<QRhiWidget::Api>
explicitBackendApi(GraphicsApi api) noexcept;
[[nodiscard]] QRhiWidget::Api primaryBackendApi() noexcept;
[[nodiscard]] QString backendApiName(QRhiWidget::Api api);
[[nodiscard]] QRhi::Implementation
rhiImplementationFor(QRhiWidget::Api api) noexcept;

} // namespace pci
