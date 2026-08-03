#pragma once

#include <cstdint>
#include <string_view>

namespace pci {

enum class GraphicsApi : std::uint8_t {
    Auto,
    Metal,
    Vulkan,
    Direct3D11,
    Direct3D12,
    OpenGL,
};

[[nodiscard]] constexpr std::string_view
graphicsApiName(const GraphicsApi api) noexcept
{
    switch (api) {
    case GraphicsApi::Auto:
        return "auto";
    case GraphicsApi::Metal:
        return "metal";
    case GraphicsApi::Vulkan:
        return "vulkan";
    case GraphicsApi::Direct3D11:
        return "d3d11";
    case GraphicsApi::Direct3D12:
        return "d3d12";
    case GraphicsApi::OpenGL:
        return "opengl";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool
graphicsApiSupportedOnPlatform(const GraphicsApi api) noexcept
{
    if (api == GraphicsApi::Auto || api == GraphicsApi::OpenGL ||
        api == GraphicsApi::Vulkan) {
        return true;
    }
#if defined(__APPLE__)
    return api == GraphicsApi::Metal;
#elif defined(_WIN32)
    return api == GraphicsApi::Direct3D11 || api == GraphicsApi::Direct3D12;
#else
    return false;
#endif
}

} // namespace pci
