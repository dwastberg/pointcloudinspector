#include "renderer/planning/PointCloudSourceBoundsIndex.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace pci {
namespace {

Bounds3d combined(const std::span<const PointCloudSourceBoundsEntry> entries)
{
    Bounds3d result = entries.front().bounds;
    for (const PointCloudSourceBoundsEntry &entry : entries.subspan(1)) {
        result.extend(entry.bounds);
    }
    return result;
}

} // namespace

void PointCloudSourceBoundsIndex::rebuild(
    const std::span<const PointCloudSourceBoundsEntry> entries)
{
    entries_.assign(entries.begin(), entries.end());
    nodes_.clear();
    if (!entries_.empty()) {
        nodes_.reserve(entries_.size() * 2U);
        static_cast<void>(buildNode(0, entries_.size()));
    }
}

std::vector<std::uint64_t>
PointCloudSourceBoundsIndex::query(const VisibilityTest &visible) const
{
    if (!visible) {
        throw std::invalid_argument(
            "source bounds query requires a visibility test");
    }
    std::vector<std::uint64_t> result;
    result.reserve(entries_.size());
    if (!nodes_.empty()) {
        queryNode(0, visible, result);
    }
    return result;
}

std::size_t PointCloudSourceBoundsIndex::size() const noexcept
{
    return entries_.size();
}

std::size_t PointCloudSourceBoundsIndex::buildNode(const std::size_t begin,
                                                   const std::size_t end)
{
    const std::size_t nodeIndex = nodes_.size();
    nodes_.push_back({
        .bounds = combined(
            std::span<const PointCloudSourceBoundsEntry>{entries_}.subspan(
                begin, end - begin)),
        .begin = begin,
        .end = end,
    });
    constexpr std::size_t leafCapacity = 4;
    if (end - begin <= leafCapacity) {
        return nodeIndex;
    }
    const Bounds3d bounds = nodes_[nodeIndex].bounds;
    const std::array extents{
        bounds.maximum[0] - bounds.minimum[0],
        bounds.maximum[1] - bounds.minimum[1],
        bounds.maximum[2] - bounds.minimum[2],
    };
    const std::size_t axis = static_cast<std::size_t>(
        std::distance(extents.begin(), std::ranges::max_element(extents)));
    const std::size_t middle = begin + (end - begin) / 2U;
    std::nth_element(entries_.begin() + static_cast<std::ptrdiff_t>(begin),
                     entries_.begin() + static_cast<std::ptrdiff_t>(middle),
                     entries_.begin() + static_cast<std::ptrdiff_t>(end),
                     [axis](const PointCloudSourceBoundsEntry &left,
                            const PointCloudSourceBoundsEntry &right) {
                         return left.bounds.center()[axis] <
                                right.bounds.center()[axis];
                     });
    const std::size_t left = buildNode(begin, middle);
    const std::size_t right = buildNode(middle, end);
    nodes_[nodeIndex].leaf = false;
    nodes_[nodeIndex].left = left;
    nodes_[nodeIndex].right = right;
    return nodeIndex;
}

void PointCloudSourceBoundsIndex::queryNode(
    const std::size_t nodeIndex,
    const VisibilityTest &visible,
    std::vector<std::uint64_t> &result) const
{
    const Node &node = nodes_[nodeIndex];
    if (!visible(node.bounds)) {
        return;
    }
    if (!node.leaf) {
        queryNode(node.left, visible, result);
        queryNode(node.right, visible, result);
        return;
    }
    for (std::size_t index = node.begin; index < node.end; ++index) {
        if (visible(entries_[index].bounds)) {
            result.push_back(entries_[index].sourceId);
        }
    }
}

} // namespace pci
