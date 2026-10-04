#pragma once

#include <pci/document/SceneDocumentSnapshot.h>
#include <pci/runtime/scene/RuntimeBudgetSnapshot.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <cstdint>

namespace pci {

enum class ViewAdjustment : std::uint8_t {
    Preserve,
    FrameVisibleLayers,
};

enum class RendererDocumentPolicy : std::uint8_t {
    Reconcile,
    ResetPointView,
};

struct DocumentUpdate {
    SessionGeneration sessionGeneration;
    SceneDocumentSnapshotPtr snapshot;
    SceneRuntimeSnapshotPtr runtime;
    RuntimeBudgetSnapshot runtimeBudget;
    ViewAdjustment viewAdjustment = ViewAdjustment::Preserve;
    RendererDocumentPolicy rendererPolicy = RendererDocumentPolicy::Reconcile;
};

} // namespace pci
