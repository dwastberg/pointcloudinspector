#pragma once

namespace pci {

struct RenderActivity {
    bool movement = false;
    bool pendingPick = false;
    bool pickReadback = false;
    bool pendingUploads = false;
    bool sceneInvalidation = false;
    bool pendingSmokeFrames = false;
};

[[nodiscard]] constexpr bool
shouldContinueRendering(const RenderActivity &activity) noexcept
{
    return activity.movement || activity.pendingPick || activity.pickReadback ||
           activity.pendingUploads || activity.sceneInvalidation ||
           activity.pendingSmokeFrames;
}

} // namespace pci
