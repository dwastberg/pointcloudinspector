#pragma once

#include "pointcloud/PointCloudMetadata.h"

#include <filesystem>

namespace pci {

class PdalSourceInspector {
public:
    [[nodiscard]] PointCloudMetadata
    inspect(const std::filesystem::path &sourcePath) const;
};

} // namespace pci
