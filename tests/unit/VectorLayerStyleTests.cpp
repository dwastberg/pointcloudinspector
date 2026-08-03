#include "vector/VectorLayerStyle.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace {

TEST_CASE("vector style defaults vary by geometry kind", "[unit][vector]")
{
    CHECK(pci::defaultVectorLayerStyle(pci::VectorGeometryKind::Polygon)
              .fill.alpha > 0.0F);
    CHECK(pci::defaultVectorLayerStyle(pci::VectorGeometryKind::Line)
              .fill.alpha == 0.0F);
    CHECK(pci::defaultVectorLayerStyle(pci::VectorGeometryKind::Point)
              .markerSizePixels == 7.0F);
}

TEST_CASE("vector style clamps invalid values", "[unit][vector]")
{
    pci::VectorLayerStyle style;
    style.opacity = std::numeric_limits<float>::infinity();
    style.strokeWidthPixels = -2.0F;
    style.markerSizePixels = 1000.0F;
    style.zOffset = std::numeric_limits<double>::quiet_NaN();
    style.markerShape = static_cast<pci::VectorMarkerShape>(42);
    const pci::VectorLayerStyle clamped = pci::clampVectorLayerStyle(style);

    CHECK(clamped.opacity == 1.0F);
    CHECK(clamped.strokeWidthPixels == 0.0F);
    CHECK(clamped.markerSizePixels == pci::maximumVectorMarkerSizePixels);
    CHECK(clamped.zOffset == 0.0);
    CHECK(clamped.markerShape == pci::VectorMarkerShape::Circle);
}

} // namespace
