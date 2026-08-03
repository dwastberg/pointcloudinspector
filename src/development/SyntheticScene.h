#pragma once

#include "scene/PointCloudScene.h"

#include <cstdint>

namespace pci {

[[nodiscard]] PointCloudScenePtr buildSyntheticScene(std::uint64_t pointCount);

} // namespace pci
