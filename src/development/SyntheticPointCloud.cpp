#include "development/SyntheticPointCloud.h"

namespace pci {
namespace {

std::uint64_t mix(std::uint64_t value)
{
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30U)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27U)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31U);
}

std::uint16_t coordinate(const std::uint64_t index, const std::uint64_t seed)
{
    return static_cast<std::uint16_t>(mix(index ^ seed) >> 48U);
}

} // namespace

GpuPoint generatePoint(const std::uint64_t index)
{
    const auto x = coordinate(index, std::uint64_t{0x243f6a8885a308d3});
    const auto y = coordinate(index, std::uint64_t{0x13198a2e03707344});
    const auto z = coordinate(index, std::uint64_t{0xa4093822299f31d0});
    const auto red = static_cast<std::uint32_t>(x >> 8U);
    const auto green = static_cast<std::uint32_t>(y >> 8U);
    const auto blue = static_cast<std::uint32_t>(z >> 8U);

    return GpuPoint{
        .x = x,
        .y = y,
        .z = z,
        .attributes = 0,
        .rgba = red | (green << 8U) | (blue << 16U) | 0xff000000U,
        .packedProperties = 0,
    };
}

std::vector<GpuPoint> generatePointChunk(const std::uint64_t firstIndex,
                                         const std::size_t count)
{
    std::vector<GpuPoint> points;
    points.reserve(count);

    for (std::size_t offset = 0; offset < count; ++offset) {
        points.push_back(generatePoint(firstIndex + offset));
    }
    return points;
}

} // namespace pci
