#include "pointcloud/CptColorMapParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

TEST_CASE("regular CPT intervals become normalized color stops",
          "[unit][color][cpt]")
{
    constexpr std::string_view cpt = R"cpt(
# PCINSPECTOR_NAME = Test relief
10 0/0/0 15 255/0/0
15 0/0/255 20 255/255/255
F 255/255/255
)cpt";

    const pci::CptColorMapParseResult result =
        pci::parseCptColorMap(cpt, "Fallback");
    REQUIRE(result);
    CHECK(result->name == "Test relief");
    REQUIRE(result->stops.size() == 4);
    CHECK(result->stops[0].position == 0.0F);
    CHECK(result->stops[1].position == Catch::Approx(0.5F));
    CHECK(result->stops[1].color.red == 1.0F);
    CHECK(result->stops[2].position == Catch::Approx(0.5F));
    CHECK(result->stops[2].color.blue == 1.0F);
    CHECK(result->stops[3].position == 1.0F);
}

TEST_CASE("CPT parser accepts separated RGB and HSV colors",
          "[unit][color][cpt]")
{
    const pci::CptColorMapParseResult rgb =
        pci::parseCptColorMap("0 0 64 255 1 255 128 0\n", "Separated RGB");
    REQUIRE(rgb);
    REQUIRE(rgb->stops.size() == 2);
    CHECK(rgb->stops.front().color.blue == 1.0F);
    CHECK(rgb->stops.back().color.red == 1.0F);

    const pci::CptColorMapParseResult hsv =
        pci::parseCptColorMap("# COLOR_MODEL = HSV\n"
                              "0 0-1-1 1 120-1-1\n",
                              "HSV");
    REQUIRE(hsv);
    CHECK(hsv->stops.front().color.red == Catch::Approx(1.0F));
    CHECK(hsv->stops.back().color.green == Catch::Approx(1.0F));

    const pci::CptColorMapParseResult constant =
        pci::parseCptColorMap("0 12/34/56 1 -\n", "Constant");
    REQUIRE(constant);
    REQUIRE(constant->stops.size() == 2);
    CHECK(constant->stops.front().color == constant->stops.back().color);
}

TEST_CASE("CPT parser reports source lines for invalid palettes",
          "[unit][color][cpt]")
{
    const pci::CptColorMapParseResult gap =
        pci::parseCptColorMap("0 0/0/0 1 255/0/0\n"
                              "2 0/0/255 3 255/255/255\n",
                              "Gap");
    REQUIRE_FALSE(gap);
    CHECK(gap.error().line == 2);
    CHECK(gap.error().message.find("contiguous") != std::string::npos);

    const pci::CptColorMapParseResult cyclic =
        pci::parseCptColorMap("# CYCLIC\n0 0/0/0 1 255/255/255\n", "Cyclic");
    REQUIRE_FALSE(cyclic);
    CHECK(cyclic.error().line == 1);
}

} // namespace
