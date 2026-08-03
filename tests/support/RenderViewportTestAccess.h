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

private:
    RenderViewportWidget &viewport_;
};

[[nodiscard]] inline RenderViewportTestAccess
testAccess(RenderViewportWidget &viewport) noexcept
{
    return RenderViewportTestAccess(viewport);
}

} // namespace pci
