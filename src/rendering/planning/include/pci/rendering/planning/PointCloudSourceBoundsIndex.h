#pragma once

#include <pci/foundation/Bounds3d.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace pci {

struct PointCloudSourceBoundsEntry {
    std::uint64_t sourceId = 0;
    Bounds3d bounds;
};

// A compact document-level BVH. It rejects groups of complete sources before
// the renderer asks any source for hierarchy descriptors or decoded pages.
class PointCloudSourceBoundsIndex final {
public:
    using VisibilityTest = std::function<bool(const Bounds3d &)>;

    void rebuild(std::span<const PointCloudSourceBoundsEntry> entries);
    [[nodiscard]] std::vector<std::uint64_t>
    query(const VisibilityTest &visible) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Node {
        Bounds3d bounds;
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t left = 0;
        std::size_t right = 0;
        bool leaf = true;
    };

    std::size_t buildNode(std::size_t begin, std::size_t end);
    void queryNode(std::size_t node,
                   const VisibilityTest &visible,
                   std::vector<std::uint64_t> &result) const;

    std::vector<PointCloudSourceBoundsEntry> entries_;
    std::vector<Node> nodes_;
};

} // namespace pci
