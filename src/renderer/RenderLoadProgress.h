#pragma once

#include "scene/SceneDocument.h"

#include <cstdint>

namespace pci {

enum class RenderLoadStage {
    Preparing,
    Uploading,
    FirstFrameReady,
    DisplayReady,
};

struct RenderLoadProgress {
    PointCloudLayerId layerId;
    RenderLoadStage stage = RenderLoadStage::Preparing;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
};

} // namespace pci
