#pragma once

#include "pointcloud/PointColorMapCatalog.h"
#include "renderer/GraphicsApi.h"
#include "renderer/RenderLoadProgress.h"
#include "renderer/RenderMetrics.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace pci {

inline constexpr int minimumPointSizePixels = 1;
inline constexpr int maximumPointSizePixels = 8;
inline constexpr int defaultPointSizePixels = 2;

inline constexpr float defaultBackgroundRed = 0.015F;
inline constexpr float defaultBackgroundGreen = 0.02F;
inline constexpr float defaultBackgroundBlue = 0.035F;
inline constexpr float minimumDepthEnhancementRadius = 0.5F;
inline constexpr float maximumDepthEnhancementRadius = 5.0F;
inline constexpr float defaultDepthEnhancementRadius = 1.0F;
inline constexpr float minimumDepthEnhancementStrength = 0.0F;
inline constexpr float maximumDepthEnhancementStrength = 100.0F;
inline constexpr float defaultDepthEnhancementStrength = 25.0F;

struct ViewportColor {
    float red = defaultBackgroundRed;
    float green = defaultBackgroundGreen;
    float blue = defaultBackgroundBlue;

    bool operator==(const ViewportColor &) const = default;
};

struct DepthEnhancementSettings {
    bool enabled = true;
    float radius = defaultDepthEnhancementRadius;
    float strength = defaultDepthEnhancementStrength;

    bool operator==(const DepthEnhancementSettings &) const = default;
};

struct ViewportSettings {
    ViewportColor backgroundColor;
    DepthEnhancementSettings depthEnhancement;

    bool operator==(const ViewportSettings &) const = default;
};

enum class ViewportTool : std::uint8_t {
    Navigate,
    Measure,
};

enum class VectorOverlayCapability : std::uint8_t {
    Unknown,
    Supported,
    Unsupported
};

class RenderViewport {
public:
    using MetricsCallback = std::function<void(const RenderMetrics &)>;
    using FailureCallback = std::function<void(const QString &)>;
    using LoadProgressCallback =
        std::function<void(const RenderLoadProgress &)>;
    using VectorOverlayCapabilityCallback =
        std::function<void(VectorOverlayCapability, const QString &)>;

    virtual ~RenderViewport() = default;

    [[nodiscard]] virtual QWidget *widget() noexcept = 0;
    [[nodiscard]] virtual QString backendName() const = 0;
    [[nodiscard]] virtual std::uint64_t totalPointCount() const noexcept = 0;
    virtual void setDocument(SceneDocumentSnapshotPtr document,
                             bool frameVisibleLayers) = 0;
    virtual void updateDocument(SceneDocumentSnapshotPtr document) = 0;
    // Schedules one frame. Implementations may coalesce repeated requests.
    virtual void requestRender() = 0;
    // Frames the camera on the combined bounds of every visible layer.
    virtual void frameVisibleLayers() = 0;
    // Frames all visible layers from a top-down view.
    virtual void frameVisibleLayersTopDown() = 0;
    [[nodiscard]] virtual bool isOrthographic() const noexcept = 0;
    virtual void setOrthographic(bool enabled) = 0;
    // Frames the camera on a single layer's bounds (e.g. "zoom to layer").
    virtual void frameLayer(PointCloudLayerId layerId) = 0;
    [[nodiscard]] virtual bool eyeDomeLightingEnabled() const noexcept = 0;
    virtual void setEyeDomeLightingEnabled(bool enabled) = 0;
    [[nodiscard]] virtual ViewportSettings
    viewportSettings() const noexcept = 0;
    virtual void setViewportSettings(const ViewportSettings &settings) = 0;
    [[nodiscard]] virtual std::uint64_t gpuByteBudget() const noexcept = 0;
    virtual void setGpuByteBudget(std::uint64_t byteBudget) = 0;
    // Raster caches are budgeted separately from the point pages. Both limits
    // apply live: lowering either evicts down to it immediately rather than at
    // the next admission.
    virtual void setRasterByteBudgets(std::uint64_t cpuByteBudget,
                                      std::uint64_t gpuByteBudget) = 0;
    [[nodiscard]] virtual int pointSizePixels() const noexcept = 0;
    virtual void setPointSizePixels(int pointSize) = 0;
    [[nodiscard]] virtual ViewportTool activeTool() const noexcept = 0;
    virtual void setActiveTool(ViewportTool tool) = 0;
    virtual void setMetricsCallback(MetricsCallback callback) = 0;
    virtual void setFailureCallback(FailureCallback callback) = 0;
    virtual void setLoadProgressCallback(LoadProgressCallback callback) = 0;
    [[nodiscard]] virtual VectorOverlayCapability
    vectorOverlayCapability() const noexcept
    {
        return VectorOverlayCapability::Unknown;
    }
    virtual void
    setVectorOverlayCapabilityCallback(VectorOverlayCapabilityCallback callback)
    {
        if (callback)
            callback(vectorOverlayCapability(), {});
    }
};

std::unique_ptr<RenderViewport> createRenderViewport(
    bool smokeTest,
    std::uint64_t gpuByteBudget = std::uint64_t{512} * 1024 * 1024,
    GraphicsApi graphicsApi = GraphicsApi::Auto,
    bool enableGpuValidation = false,
    PointColorMapCatalogSnapshotPtr colorMaps = {});

} // namespace pci
