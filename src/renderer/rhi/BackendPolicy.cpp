#include "renderer/rhi/BackendPolicy.h"

namespace pci {

std::optional<QRhiWidget::Api>
explicitBackendApi(const GraphicsApi api) noexcept
{
    switch (api) {
    case GraphicsApi::Auto:
        return std::nullopt;
    case GraphicsApi::Metal:
        return QRhiWidget::Api::Metal;
    case GraphicsApi::Vulkan:
        return QRhiWidget::Api::Vulkan;
    case GraphicsApi::Direct3D11:
        return QRhiWidget::Api::Direct3D11;
    case GraphicsApi::Direct3D12:
        return QRhiWidget::Api::Direct3D12;
    case GraphicsApi::OpenGL:
        return QRhiWidget::Api::OpenGL;
    }
    return std::nullopt;
}

QRhiWidget::Api primaryBackendApi() noexcept
{
#if defined(Q_OS_MACOS)
    return QRhiWidget::Api::Metal;
#elif defined(Q_OS_WIN)
    return QRhiWidget::Api::Direct3D12;
#else
    return QRhiWidget::Api::Vulkan;
#endif
}

QString backendApiName(const QRhiWidget::Api api)
{
    switch (api) {
    case QRhiWidget::Api::Metal:
        return QStringLiteral("Metal");
    case QRhiWidget::Api::Vulkan:
        return QStringLiteral("Vulkan");
    case QRhiWidget::Api::Direct3D12:
        return QStringLiteral("Direct3D 12");
    case QRhiWidget::Api::Direct3D11:
        return QStringLiteral("Direct3D 11");
    case QRhiWidget::Api::OpenGL:
        return QStringLiteral("OpenGL");
    case QRhiWidget::Api::Null:
        return QStringLiteral("Null");
    }
    return QStringLiteral("Unknown");
}

QRhi::Implementation rhiImplementationFor(const QRhiWidget::Api api) noexcept
{
    switch (api) {
    case QRhiWidget::Api::Metal:
        return QRhi::Metal;
    case QRhiWidget::Api::Vulkan:
        return QRhi::Vulkan;
    case QRhiWidget::Api::Direct3D12:
        return QRhi::D3D12;
    case QRhiWidget::Api::Direct3D11:
        return QRhi::D3D11;
    case QRhiWidget::Api::OpenGL:
        return QRhi::OpenGLES2;
    case QRhiWidget::Api::Null:
        return QRhi::Null;
    }
    return QRhi::Null;
}

} // namespace pci
