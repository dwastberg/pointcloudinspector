#pragma once

#include <pci/desktop/viewport/ViewportInputController.h>

#include <pci/desktop/viewport/RenderViewportWidget_p.h>

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
        return viewport_.inputController_->camera();
    }

    [[nodiscard]] const NavigationInputState &inputForTesting() const noexcept
    {
        return viewport_.inputController_->state();
    }

    void advanceKeyboardNavigationForTesting(const double deltaSeconds)
    {
        viewport_.applyKeyboardNavigation(deltaSeconds);
    }

    void requestRawPickForTesting(QPoint position,
                                  PointPicker::Completion completion)
    {
        viewport_.rawPickCompletion_ = std::move(completion);
        viewport_.inputController_->state().queueRaw(
            {position.x(), position.y()});
        viewport_.requestRender();
    }

    [[nodiscard]] std::uint64_t renderedFrameCountForTesting() const noexcept
    {
        return viewport_.frameExecutor_.telemetry().frameCount();
    }

    [[nodiscard]] bool eyeDomeLightingActiveForTesting() const noexcept
    {
        return viewport_.eyeDomeLightingActive_;
    }

    [[nodiscard]] std::uint64_t
    residentLayerPointsForTesting(PointCloudLayerId layerId) const
    {
        return viewport_.renderer_.uploads.residentPointCount(layerId);
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
        viewport_.inputController_->camera().frameTopDown(distanceMultiplier);
        viewport_.requestRender();
    }

    void frameTopDownAtForTesting(const Vec3d center,
                                  const double sceneDiameter,
                                  const double distanceMultiplier)
    {
        viewport_.inputController_->camera().setScene(center, sceneDiameter);
        viewport_.inputController_->camera().frameTopDown(distanceMultiplier);
        viewport_.requestRender();
    }

    void orbitCameraForTesting(const double horizontalPixels,
                               const double verticalPixels)
    {
        viewport_.inputController_->camera().orbitFromDrag(horizontalPixels,
                                                           verticalPixels);
        viewport_.requestRender();
    }

    [[nodiscard]] QMatrix4x4 frameViewProjectionForTesting() const
    {
        const FrameCamera frame = viewport_.currentFrameCamera();
        return FrameRenderer::viewProjection(viewport_.rhi(), frame);
    }

    [[nodiscard]] std::size_t rasterResidentTilesForTesting() const noexcept
    {
        return viewport_.renderer_.rasters.residentTileCount();
    }

    [[nodiscard]] std::size_t rasterDrawnTilesForTesting() const noexcept
    {
        return viewport_.rasterDrawnTiles_;
    }

    [[nodiscard]] std::size_t rasterSurfaceDrawnTilesForTesting() const noexcept
    {
        return viewport_.rasterSurfaceDrawnTiles_;
    }

    [[nodiscard]] std::uint64_t rasterHeightGpuBytesForTesting() const noexcept
    {
        return viewport_.renderer_.rasters.heightGpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterGpuBytesForTesting() const noexcept
    {
        return viewport_.renderer_.rasters.gpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterGpuPeakBytesForTesting() const noexcept
    {
        return viewport_.renderer_.rasters.peakGpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterCpuBytesForTesting() const noexcept
    {
        return viewport_.frameExecutor_.rasters().metrics().cpuBytes;
    }

    [[nodiscard]] QRhi *rhiForTesting() const noexcept
    {
        return viewport_.rhi();
    }

    // A settled single-point scene supplies real uniforms and a vertex buffer.
    // Recording into an explicit offscreen frame lets lifetime tests tear down
    // the picker before QRhi's end-of-frame readback delivery.
    void recordSinglePointPickForTesting(QRhiCommandBuffer *commandBuffer,
                                         PointPicker &picker,
                                         QSize pixelSize,
                                         PointPicker::Completion completion)
    {
        auto &renderer = viewport_.renderer_;
        picker.ensureResources(viewport_.rhi(),
                               renderer.points.shaderBindings());
        const std::vector<BlockDraw> draws{{
            .buffer = renderer.uploads.bufferFor(
                viewport_.currentGpuProtection_.at(0)),
            .pointCount = 1,
        }};
        picker.record(commandBuffer,
                      renderer.points.shaderBindings(),
                      draws,
                      renderer.points.uniformStride(),
                      QPoint(pixelSize.width() / 2, pixelSize.height() / 2),
                      pixelSize,
                      2,
                      std::move(completion));
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
