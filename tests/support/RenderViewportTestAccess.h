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

    [[nodiscard]] bool fullDetailWarmingForTesting() const noexcept
    {
        return viewport_.pointFrameCoordinator_.fullDetailPlanned() &&
               !viewport_.pointFrameCoordinator_.fullDetailActive();
    }

    [[nodiscard]] bool fullDetailActiveForTesting() const noexcept
    {
        return viewport_.pointFrameCoordinator_.fullDetailActive();
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

    // Re-frames top-down at a chosen distance so a test can sweep zoom levels
    // without synthesizing wheel events. The scene extent must already be set,
    // which frameVisibleLayersTopDown() does.
    void frameTopDownForTesting(const double distanceMultiplier)
    {
        viewport_.camera_.frameTopDown(distanceMultiplier);
        viewport_.requestRender();
    }

    [[nodiscard]] std::size_t rasterResidentTilesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.residentTileCount();
    }

    [[nodiscard]] std::uint64_t rasterGpuBytesForTesting() const noexcept
    {
        return viewport_.rasterLayerRenderer_.gpuBytes();
    }

    [[nodiscard]] std::uint64_t rasterCpuBytesForTesting() const noexcept
    {
        return viewport_.rasterTileStreamer_.metrics().cpuBytes;
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
