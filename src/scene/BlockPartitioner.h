#pragma once

#include "scene/PointBlock.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace pci {

struct PointSample {
    Vec3d position;
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t packedAttributes = 0;
    std::uint32_t packedProperties = 0;
    PointAttributes attributes;
};

class BlockPartitioner {
public:
    using BlockReady = std::function<void(PointBlockPtr)>;

    BlockPartitioner(const Bounds3d &bounds,
                     std::uint64_t expectedPointCount,
                     BlockReady blockReady,
                     std::size_t initialBlockCapacity = 0);

    void add(const PointSample &sample);
    // Publishes up to maximumBlocks of the largest active cells. This bounds
    // sparse-stream latency without flushing every tiny cell into a GPU buffer.
    [[nodiscard]] std::size_t flushLargest(std::size_t maximumBlocks);
    void finish();
    [[nodiscard]] double cellEdge() const noexcept;

private:
    [[nodiscard]] std::uint64_t cellKeyFor(Vec3d position) const noexcept;
    [[nodiscard]] Vec3d cellOrigin(std::uint64_t key) const noexcept;
    void seal(std::shared_ptr<PointBlock> &block);

    Bounds3d bounds_;
    double edge_ = 1.0;
    std::size_t initialBlockCapacity_ = 0;
    std::array<std::uint64_t, 3> cellCounts_{1, 1, 1};
    BlockReady blockReady_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PointBlock>> building_;
};

} // namespace pci
