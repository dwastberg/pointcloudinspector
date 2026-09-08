#pragma once

#include "renderer/rhi/RenderViewportWidget_p.h"

#include <utility>

namespace pci {

// Renderer-internal tests use this friend fixture instead of adding
// test-only inspection methods to the production widget API.
class RenderViewportTestAccess {
public:
    explicit RenderViewportTestAccess(RenderViewportWidget &viewport) noexcept
        : viewport_(viewport)
    {
    }

    [[nodiscard]] const NavigationCamera &cameraForTesting() const noexcept
    {
        return viewport_.camera_;
    }

    [[nodiscard]] const NavigationInputState &inputForTesting() const noexcept
    {
        return viewport_.input_;
    }

    void advanceKeyboardNavigationForTesting(const double deltaSeconds)
    {
        viewport_.applyKeyboardNavigation(deltaSeconds);
    }

    void requestRawPickForTesting(QPoint position,
                                  PointPicker::Completion completion)
    {
        viewport_.rawPickCompletion_ = std::move(completion);
        viewport_.input_.queueRaw({position.x(), position.y()});
        viewport_.requestRender();
    }

    [[nodiscard]] std::uint64_t renderedFrameCountForTesting() const noexcept
    {
        return viewport_.telemetry_.frameCount();
    }

    [[nodiscard]] bool eyeDomeLightingActiveForTesting() const noexcept
    {
        return viewport_.eyeDomeLightingActive_;
    }

    [[nodiscard]] std::uint64_t
    residentLayerPointsForTesting(PointCloudLayerId layerId) const
    {
        return viewport_.uploadScheduler_.residentPointCount(layerId);
    }

    [[nodiscard]] const std::optional<DistanceMeasurement> &
    measurementForTesting() const noexcept
    {
        return viewport_.measurementController_.state().measurement;
    }

    [[nodiscard]] bool measurementHasAnchorForTesting() const noexcept
    {
        return viewport_.measurementController_.state().anchor.has_value();
    }

    // Re-frames top-down at a chosen distance so a test can sweep zoom levels
    // without synthesizing wheel events. The scene extent must already be set,
    // which frameVisibleLayersTopDown() does.
    void frameTopDownForTesting(const double distanceMultiplier)
    {
        viewport_.camera_.frameTopDown(distanceMultiplier);
        viewport_.requestRender();
    }

    void frameTopDownAtForTesting(const Vec3d center,
                                  const double sceneDiameter,
                                  const double distanceMultiplier)
    {
        viewport_.camera_.setScene(center, sceneDiameter);
        viewport_.camera_.frameTopDown(distanceMultiplier);
        viewport_.requestRender();
    }

    void orbitCameraForTesting(const double horizontalPixels,
                               const double verticalPixels)
    {
        viewport_.camera_.orbitFromDrag(horizontalPixels, verticalPixels);
        viewport_.requestRender();
    }

    [[nodiscard]] QMatrix4x4 frameViewProjectionForTesting() const
    {
        const FrameCamera frame = viewport_.currentFrameCamera();
        return viewport_.frameViewProjection(frame);
    }

    [[nodiscard]] std::size_t rasterResidentTilesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.residentTileCount();
    }

    [[nodiscard]] std::size_t rasterDrawnTilesForTesting() const noexcept
    {
        return viewport_.rasterDrawnTiles_;
    }

    [[nodiscard]] std::size_t
    rasterSurfaceDrawnTilesForTesting() const noexcept
    {
        return viewport_.rasterSurfaceDrawnTiles_;
    }

    [[nodiscard]] std::uint64_t
    rasterHeightGpuBytesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.heightGpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterGpuBytesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.gpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterGpuPeakBytesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.peakGpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterCpuBytesForTesting() const noexcept
    {
        return viewport_.rasterTileStreamer_.metrics().cpuBytes;
    }

    void releaseResourcesForTesting()
    {
        viewport_.releaseResources();
    }

private:
    RenderViewportWidget &viewport_;
};

[[nodiscard]] inline RenderViewportTestAccess
testAccess(RenderViewportWidget &viewport) noexcept
{
    return RenderViewportTestAccess(viewport);
}

} // namespace pci
