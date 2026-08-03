#pragma once

#include "foundation/Vec3d.h"

#include <cstdint>
#include <optional>

namespace pci {

class NavigationCamera {
public:
    static constexpr double verticalFieldOfViewDegrees = 60.0;
    static constexpr Vec3d worldUp{0.0, 0.0, 1.0};

    struct ClipPlanes {
        double nearPlane;
        double farPlane;
    };

    [[nodiscard]] Vec3d position() const noexcept;
    [[nodiscard]] Vec3d pivot() const noexcept;
    [[nodiscard]] Vec3d forward() const noexcept;
    [[nodiscard]] Vec3d right() const noexcept;
    [[nodiscard]] Vec3d up() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

    void setScene(Vec3d center, double diameter) noexcept;
    [[nodiscard]] double sceneDiameter() const noexcept;
    void frameScene() noexcept;
    // Frames the scene using an explicit camera-distance multiplier. Values
    // below the historical default of 2.0 produce a tighter viewport fit.
    void frameScene(double distanceMultiplier) noexcept;
    void frameTopDown(double distanceMultiplier = 2.0) noexcept;
    void setOrthographic(bool enabled) noexcept;
    [[nodiscard]] bool isOrthographic() const noexcept;
    [[nodiscard]] double orthographicScale() const noexcept;
    void setPivot(Vec3d pivot) noexcept;
    void setNavigationReference(Vec3d point) noexcept;
    void orbitFromDrag(double horizontalPixels, double verticalPixels) noexcept;
    void panFromDrag(double horizontalPixels,
                     double verticalPixels,
                     double viewportHeightPixels) noexcept;
    void translate(Vec3d worldDelta) noexcept;
    void dollyToward(Vec3d target, double wheelUnits) noexcept;
    void dollyForward(double wheelUnits) noexcept;

    [[nodiscard]] double movementSpeed(double multiplier = 1.0) const noexcept;
    [[nodiscard]] ClipPlanes clipPlanes() const noexcept;

private:
    void markChanged() noexcept;

    Vec3d position_{0.0, -4.0, 0.0};
    Vec3d pivot_{};
    Vec3d forward_{0.0, 1.0, 0.0};
    Vec3d sceneCenter_{};
    double sceneDiameter_ = 2.0;
    std::optional<Vec3d> navigationReference_;
    bool orthographic_ = false;
    double orthographicScale_ = 2.0;
    std::uint64_t revision_ = 0;
};

} // namespace pci
