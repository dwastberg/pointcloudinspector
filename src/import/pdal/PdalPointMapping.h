#pragma once

#include "pointcloud/PointCloudMetadata.h"
#include "scene/BlockPartitioner.h"

namespace pdal {
class PointRef;
}

namespace pci {

[[nodiscard]] PointSample mapPdalPoint(const pdal::PointRef &point,
                                       const PointCloudMetadata &metadata);

} // namespace pci
