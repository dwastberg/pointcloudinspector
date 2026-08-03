#pragma once

#include "foundation/Vec3d.h"

#include <array>

namespace pci {

struct Bounds3d {
    std::array<double, 3> minimum{};
    std::array<double, 3> maximum{};

    void extend(const Bounds3d &bounds) noexcept;
    void extend(Vec3d point) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::array<double, 3> center() const noexcept;
    [[nodiscard]] double maximumExtent() const noexcept;
};

} // namespace pci
