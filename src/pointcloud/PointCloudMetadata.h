#pragma once

#include "foundation/Bounds3d.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pci {

struct PointCloudMetadata {
    std::filesystem::path sourcePath;
    std::string sourceDriver;
    std::string spatialReferenceWkt;
    std::uint64_t sourcePointCount = 0;
    Bounds3d sourceBounds;
    std::vector<std::string> dimensions;
    bool hasColor = false;
    bool hasIntensity = false;
    bool hasClassification = false;
    bool hasReturnNumber = false;
    bool hasNumberOfReturns = false;
};

} // namespace pci
