#pragma once

#include <pci/foundation/Bounds3d.h>
#include <pci/foundation/Vec3d.h>
#include <pci/vector/VectorGeometry.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pci {

struct VectorLayerData {
    Vec3d origin;
    std::vector<VectorFillBatch> fillBatches;
    std::vector<VectorSegment2f> segments;
    std::vector<VectorVertex2f> markers;
    Bounds3d bounds;
    VectorGeometrySummary summary;
    std::uint64_t featureCount = 0;
    std::filesystem::path sourcePath;
    std::string sourceDriver;
    std::string sublayerName;
    std::string spatialReferenceWkt;
    bool crsMismatch = false;
    bool extentDisjointXY = false;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint64_t byteSize() const noexcept;
    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;
    [[nodiscard]] std::uint64_t triangleCount() const noexcept;
    [[nodiscard]] std::string geometryDescription() const;
};

using VectorLayerDataPtr = std::shared_ptr<const VectorLayerData>;

} // namespace pci
