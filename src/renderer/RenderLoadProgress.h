#pragma once

#include "scene/SceneDocument.h"

#include <cstdint>

namespace pci {

enum class RenderLoadStage {
    Preparing,
    Uploading,
    FirstFrameReady,
    FullDetailWarming,
    DisplayReady,
};

struct RenderLoadProgress {
    PointCloudLayerId layerId;
    RenderLoadStage stage = RenderLoadStage::Preparing;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t decoded = 0;
    std::uint64_t uploaded = 0;
};

} // namespace pci
