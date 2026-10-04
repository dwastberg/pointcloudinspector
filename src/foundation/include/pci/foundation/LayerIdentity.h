#pragma once

#include <pci/foundation/StrongId.h>

namespace pci {

struct SceneLayerTag;
using SceneLayerId = StrongId<SceneLayerTag>;
using PointCloudLayerId = SceneLayerId;

} // namespace pci
