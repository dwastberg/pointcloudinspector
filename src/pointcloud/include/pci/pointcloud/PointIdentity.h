#pragma once

#include <pci/foundation/StrongId.h>

namespace pci {

struct PointCloudSourceIdTag;
using PointCloudSourceId = StrongId<PointCloudSourceIdTag>;

[[nodiscard]] PointCloudSourceId allocatePointCloudSourceId();

} // namespace pci
