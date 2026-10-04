#pragma once

#include <pci/pointcloud/BlockPartitioner.h>
#include <pci/pointcloud/PointCloudMetadata.h>

namespace pdal {
class PointRef;
}

namespace pci {

[[nodiscard]] PointSample mapPdalPoint(const pdal::PointRef &point,
                                       const PointCloudMetadata &metadata);

} // namespace pci
