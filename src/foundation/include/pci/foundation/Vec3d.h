#pragma once

#include <cmath>

namespace pci {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool operator==(const Vec3d &) const = default;
};

[[nodiscard]] constexpr Vec3d operator+(const Vec3d left,
                                        const Vec3d right) noexcept
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] constexpr Vec3d operator-(const Vec3d left,
                                        const Vec3d right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] constexpr Vec3d operator*(const Vec3d value,
                                        const double scalar) noexcept
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] constexpr Vec3d operator*(const double scalar,
                                        const Vec3d value) noexcept
{
    return value * scalar;
}

[[nodiscard]] constexpr Vec3d operator/(const Vec3d value,
                                        const double scalar) noexcept
{
    return {value.x / scalar, value.y / scalar, value.z / scalar};
}

[[nodiscard]] constexpr double dot(const Vec3d left, const Vec3d right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] constexpr Vec3d cross(const Vec3d left,
                                    const Vec3d right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] inline double length(const Vec3d value) noexcept
{
    return std::sqrt(dot(value, value));
}

[[nodiscard]] inline Vec3d normalized(const Vec3d value) noexcept
{
    const double magnitude = length(value);
    return magnitude > 0.0 ? value / magnitude : Vec3d{};
}

[[nodiscard]] inline Vec3d rotateAroundAxis(const Vec3d value,
                                            const Vec3d axis,
                                            const double radians) noexcept
{
    const Vec3d unitAxis = normalized(axis);
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return value * cosine + cross(unitAxis, value) * sine +
           unitAxis * dot(unitAxis, value) * (1.0 - cosine);
}

[[nodiscard]] inline bool isFinite(const Vec3d value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

} // namespace pci
