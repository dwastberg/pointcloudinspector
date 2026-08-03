#include "renderer/rhi/BackendPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("primary backend api matches the build platform", "[qt][renderer]")
{
#if defined(Q_OS_MACOS)
    CHECK(pci::primaryBackendApi() == QRhiWidget::Api::Metal);
#elif defined(Q_OS_WIN)
    CHECK(pci::primaryBackendApi() == QRhiWidget::Api::Direct3D12);
#else
    CHECK(pci::primaryBackendApi() == QRhiWidget::Api::Vulkan);
#endif
}

TEST_CASE("automatic backend selection does not force a QRhi API",
          "[qt][renderer]")
{
    CHECK_FALSE(pci::explicitBackendApi(pci::GraphicsApi::Auto));
    CHECK(pci::explicitBackendApi(pci::GraphicsApi::Metal) ==
          QRhiWidget::Api::Metal);
    CHECK(pci::explicitBackendApi(pci::GraphicsApi::Vulkan) ==
          QRhiWidget::Api::Vulkan);
    CHECK(pci::explicitBackendApi(pci::GraphicsApi::Direct3D11) ==
          QRhiWidget::Api::Direct3D11);
    CHECK(pci::explicitBackendApi(pci::GraphicsApi::Direct3D12) ==
          QRhiWidget::Api::Direct3D12);
    CHECK(pci::explicitBackendApi(pci::GraphicsApi::OpenGL) ==
          QRhiWidget::Api::OpenGL);
}

TEST_CASE("backend policy rejects APIs that cannot exist on the platform",
          "[qt][renderer]")
{
    CHECK(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Auto));
    CHECK(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::OpenGL));
    CHECK(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Vulkan));
#if defined(Q_OS_MACOS)
    CHECK(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Metal));
    CHECK_FALSE(
        pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Direct3D11));
#elif defined(Q_OS_WIN)
    CHECK_FALSE(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Metal));
    CHECK(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Direct3D11));
#else
    CHECK_FALSE(pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Metal));
    CHECK_FALSE(
        pci::graphicsApiSupportedOnPlatform(pci::GraphicsApi::Direct3D12));
#endif
}

TEST_CASE("backend api names and implementations are consistent",
          "[qt][renderer]")
{
    CHECK(pci::backendApiName(QRhiWidget::Api::Metal) ==
          QStringLiteral("Metal"));
    CHECK(pci::backendApiName(QRhiWidget::Api::Vulkan) ==
          QStringLiteral("Vulkan"));
    CHECK(pci::backendApiName(QRhiWidget::Api::Direct3D12) ==
          QStringLiteral("Direct3D 12"));
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Metal) == QRhi::Metal);
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Vulkan) == QRhi::Vulkan);
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Direct3D12) ==
          QRhi::D3D12);
}

} // namespace
