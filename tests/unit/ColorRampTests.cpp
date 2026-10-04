#include <pci/color/ColorRamp.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

void checkColor(const pci::PointRgba actual, const pci::PointRgba expected)
{
    CHECK(actual.red == Catch::Approx(expected.red));
    CHECK(actual.green == Catch::Approx(expected.green));
    CHECK(actual.blue == Catch::Approx(expected.blue));
    CHECK(actual.alpha == Catch::Approx(expected.alpha));
}

TEST_CASE("color ramp interpolation defines edge and empty behavior",
          "[unit][color][ramp]")
{
    const pci::PointRgba fallback{0.2F, 0.3F, 0.4F, 0.5F};
    CHECK(pci::interpolateColorRamp({}, 0.25F, fallback) == fallback);
    CHECK(pci::interpolateColorRamp({}, 0.25, fallback) == fallback);

    const std::vector<pci::PointColorStop> single{
        {0.4F, {0.7F, 0.6F, 0.5F, 0.4F}},
    };
    CHECK(pci::interpolateColorRamp(single, -1.0F, fallback) ==
          single.front().color);
    CHECK(pci::interpolateColorRamp(single, 2.0, fallback) ==
          single.front().color);

    const std::vector<pci::PointColorStop> stops{
        {0.0F, {0.1F, 0.2F, 0.3F, 0.4F}},
        {0.5F, {0.9F, 0.8F, 0.7F, 0.6F}},
        {0.5F, {0.2F, 0.4F, 0.6F, 0.8F}},
        {1.0F, {1.0F, 0.9F, 0.8F, 0.7F}},
    };
    CHECK(pci::interpolateColorRamp(stops, -1.0F, fallback) ==
          stops.front().color);
    CHECK(pci::interpolateColorRamp(stops, 2.0, fallback) ==
          stops.back().color);
    // At a duplicated position, the first stop owns the exact boundary. Just
    // above it, interpolation starts from the last duplicate.
    CHECK(pci::interpolateColorRamp(stops, 0.5, fallback) == stops[1].color);
    const pci::PointRgba above =
        pci::interpolateColorRamp(stops, 0.5001, fallback);
    CHECK(above.red > stops[2].color.red);
    CHECK(above.red < stops.back().color.red);
}

TEST_CASE("color ramp overloads retain caller arithmetic precision",
          "[unit][color][ramp][precision]")
{
    const std::vector<pci::PointColorStop> stops{
        {0.1F, {0.1234567F, 0.8F, 0.1F, 0.25F}},
        {0.9F, {0.8765432F, 0.2F, 0.9F, 0.75F}},
    };
    constexpr float floatPosition = 0.33333334F;
    const float floatFraction = (floatPosition - stops[0].position) /
                                (stops[1].position - stops[0].position);
    const pci::PointRgba asFloat =
        pci::interpolateColorRamp(stops, floatPosition, {});
    CHECK(asFloat.red ==
          std::lerp(stops[0].color.red, stops[1].color.red, floatFraction));

    constexpr double doublePosition = 1.0 / 3.0;
    const double doubleFraction =
        (doublePosition - static_cast<double>(stops[0].position)) /
        (static_cast<double>(stops[1].position) - stops[0].position);
    const pci::PointRgba asDouble =
        pci::interpolateColorRamp(stops, doublePosition, {});
    CHECK(asDouble.red ==
          static_cast<float>(
              static_cast<double>(stops[0].color.red) +
              (static_cast<double>(stops[1].color.red) - stops[0].color.red) *
                  doubleFraction));
}

TEST_CASE("color ramp interpolates every RGBA channel", "[unit][color][ramp]")
{
    const std::vector<pci::PointColorStop> stops{
        {0.0F, {0.0F, 0.2F, 0.4F, 0.6F}},
        {1.0F, {1.0F, 0.8F, 0.6F, 0.4F}},
    };
    checkColor(pci::interpolateColorRamp(stops, 0.25F, {}),
               {0.25F, 0.35F, 0.45F, 0.55F});
    checkColor(pci::interpolateColorRamp(stops, 0.75, {}),
               {0.75F, 0.65F, 0.55F, 0.45F});
}

} // namespace
