#include <pci/color/ColorRamp.h>

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

template <typename Position, typename Mix>
PointRgba interpolate(const std::span<const PointColorStop> stops,
                      const Position position,
                      const PointRgba emptyFallback,
                      Mix mix) noexcept
{
    if (stops.empty()) {
        return emptyFallback;
    }
    if (position <= static_cast<Position>(stops.front().position)) {
        return stops.front().color;
    }
    if (position >= static_cast<Position>(stops.back().position)) {
        return stops.back().color;
    }
    const auto upper =
        std::ranges::find_if(stops, [position](const PointColorStop &stop) {
            return static_cast<Position>(stop.position) >= position;
        });
    if (upper == stops.end()) {
        return stops.back().color;
    }

    const PointColorStop &right = *upper;
    const PointColorStop &left = *(upper - 1);
    const Position span = static_cast<Position>(right.position) -
                          static_cast<Position>(left.position);
    const Position fraction =
        span > Position{}
            ? (position - static_cast<Position>(left.position)) / span
            : Position{};
    return {
        .red = mix(left.color.red, right.color.red, fraction),
        .green = mix(left.color.green, right.color.green, fraction),
        .blue = mix(left.color.blue, right.color.blue, fraction),
        .alpha = mix(left.color.alpha, right.color.alpha, fraction),
    };
}

} // namespace

PointRgba interpolateColorRamp(const std::span<const PointColorStop> stops,
                               const float position,
                               const PointRgba emptyFallback) noexcept
{
    return interpolate(
        stops,
        position,
        emptyFallback,
        [](const float left, const float right, const float fraction) {
            return std::lerp(left, right, fraction);
        });
}

PointRgba interpolateColorRamp(const std::span<const PointColorStop> stops,
                               const double position,
                               const PointRgba emptyFallback) noexcept
{
    return interpolate(
        stops,
        position,
        emptyFallback,
        [](const float left, const float right, const double fraction) {
            return static_cast<float>(static_cast<double>(left) +
                                      (static_cast<double>(right) - left) *
                                          fraction);
        });
}

} // namespace pci
