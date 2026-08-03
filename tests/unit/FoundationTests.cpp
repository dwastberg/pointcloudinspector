#include "foundation/Bounds3d.h"
#include "foundation/CheckedArithmetic.h"
#include "foundation/Hash.h"
#include "foundation/StrongId.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace {

static_assert(pci::saturatingAdd(1U, 2U) == 3U);
static_assert(pci::saturatingMultiply(2U, 3U) == 6U);
static_assert(pci::checkedAdd(1U, 2U).value() == 3U);
static_assert(pci::checkedMultiply(2U, 3U).value() == 6U);
static_assert(pci::hashCombine(1, 2) == pci::hashCombine(1, 2));

struct FirstIdTag;
struct SecondIdTag;
using FirstId = pci::StrongId<FirstIdTag>;
using SecondId = pci::StrongId<SecondIdTag>;

static_assert(std::is_constructible_v<FirstId, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, FirstId>);
static_assert(!std::is_convertible_v<FirstId, std::uint64_t>);
static_assert(!std::is_convertible_v<FirstId, SecondId>);
static_assert(!std::is_constructible_v<FirstId, SecondId>);

TEST_CASE("strong identifiers preserve tag and representation semantics",
          "[unit][foundation][strong-id]")
{
    const FirstId first{17};
    const FirstId same{17};
    const FirstId later{23};

    CHECK(first.value() == 17);
    CHECK(first == same);
    CHECK(first < later);

    const std::unordered_set<FirstId> identifiers{first, same, later};
    CHECK(identifiers.size() == 2);
    CHECK(identifiers.contains(FirstId{17}));
}

TEST_CASE("checked and saturating arithmetic handles unsigned boundaries",
          "[unit][foundation][overflow]")
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

    CHECK(pci::saturatingAdd<std::uint64_t>(0, 0) == 0);
    CHECK(pci::saturatingAdd<std::uint64_t>(maximum - 1, 1) == maximum);
    CHECK(pci::saturatingAdd<std::uint64_t>(maximum, 1) == maximum);
    CHECK(pci::saturatingMultiply<std::uint64_t>(0, maximum) == 0);
    CHECK(pci::saturatingMultiply<std::uint64_t>(maximum, 1) == maximum);
    CHECK(pci::saturatingMultiply<std::uint64_t>(maximum, 2) == maximum);

    CHECK(pci::checkedAdd<std::uint64_t>(0, 0) == 0);
    CHECK(pci::checkedAdd<std::uint64_t>(maximum - 1, 1) == maximum);
    CHECK_FALSE(pci::checkedAdd<std::uint64_t>(maximum, 1));
    CHECK(pci::checkedMultiply<std::uint64_t>(0, maximum) == 0);
    CHECK(pci::checkedMultiply<std::uint64_t>(maximum, 1) == maximum);
    CHECK_FALSE(pci::checkedMultiply<std::uint64_t>(maximum, 2));
}

TEST_CASE("bounds validate, extend, and measure finite extents",
          "[unit][foundation][bounds]")
{
    pci::Bounds3d bounds{
        .minimum = {1000.0, 2000.0, 10.0},
        .maximum = {1010.0, 2020.0, 15.0},
    };

    CHECK(bounds.valid());
    CHECK(bounds.center() == std::array{1005.0, 2010.0, 12.5});
    CHECK(bounds.maximumExtent() == Catch::Approx(20.0));

    bounds.extend(pci::Vec3d{999.0, 2025.0, 12.0});
    bounds.extend(
        {.minimum = {1001.0, 1990.0, -5.0}, .maximum = {1020.0, 2010.0, 25.0}});
    CHECK(bounds.minimum == std::array{999.0, 1990.0, -5.0});
    CHECK(bounds.maximum == std::array{1020.0, 2025.0, 25.0});

    const pci::Bounds3d reversed{
        .minimum = {1.0, 0.0, 0.0},
        .maximum = {0.0, 1.0, 1.0},
    };
    CHECK_FALSE(reversed.valid());
    const pci::Bounds3d infinite{
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {INFINITY, 1.0, 1.0},
    };
    CHECK_FALSE(infinite.valid());
}

TEST_CASE("degenerate bounds remain usable", "[unit][foundation][bounds]")
{
    const pci::Bounds3d pointBounds{
        .minimum = {4.0, 5.0, 6.0},
        .maximum = {4.0, 5.0, 6.0},
    };

    CHECK(pointBounds.valid());
    CHECK(pointBounds.center() == pointBounds.minimum);
    CHECK(pointBounds.maximumExtent() == 0.0);
}

} // namespace
