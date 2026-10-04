#include <pci/development/SyntheticScene.h>

#include <pci/development/SyntheticPointCloud.h>

#include <algorithm>

namespace pci {
namespace {

constexpr Bounds3d normalizedCube{
    .minimum = {-1.0, -1.0, -1.0},
    .maximum = {1.0, 1.0, 1.0},
};

} // namespace

PointDatasetRuntimePtr buildSyntheticScene(const std::uint64_t pointCount)
{
    PointCloudMetadata metadata;
    metadata.sourceDriver = "synthetic";
    metadata.sourcePointCount = pointCount;
    metadata.sourceBounds = normalizedCube;
    metadata.hasColor = true;

    auto scene = std::make_shared<PointDatasetRuntime>(std::move(metadata));
    for (std::uint64_t first = 0; first < pointCount;
         first += maximumPointsPerBlock) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(maximumPointsPerBlock, pointCount - first));
        auto block = std::make_shared<PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / blockQuantizationSteps;
        block->bounds = normalizedCube;
        block->points = generatePointChunk(first, count);
        block->attributes.resize(count);
        scene->addBlock(std::move(block));
    }
    scene->markLoadingComplete();
    return scene;
}

} // namespace pci
