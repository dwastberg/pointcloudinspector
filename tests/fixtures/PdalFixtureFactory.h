#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace pci::test {

struct FixturePoint {
    double x;
    double y;
    double z;
    std::uint16_t red;
    std::uint16_t green;
    std::uint16_t blue;
    std::uint16_t intensity;
    std::uint8_t classification;
    std::uint8_t returnNumber;
    std::uint8_t numberOfReturns;
};

inline constexpr std::array<FixturePoint, 8> fixturePoints{{
    {1000.0, 2000.0, 10.0, 65535, 0, 0, 100, 2, 1, 2},
    {1010.0, 2000.0, 10.0, 0, 65535, 0, 200, 2, 2, 2},
    {1000.0, 2010.0, 10.0, 0, 0, 65535, 300, 5, 1, 1},
    {1010.0, 2010.0, 10.0, 65535, 65535, 0, 400, 5, 1, 3},
    {1000.0, 2000.0, 20.0, 65535, 0, 65535, 500, 6, 2, 3},
    {1010.0, 2000.0, 20.0, 0, 65535, 65535, 600, 6, 3, 3},
    {1000.0, 2010.0, 20.0, 32768, 32768, 32768, 700, 1, 1, 4},
    {1010.0, 2010.0, 20.0, 65535, 65535, 65535, 800, 1, 4, 4},
}};

struct PdalFixturePaths {
    std::filesystem::path las;
    std::filesystem::path laz;
    std::filesystem::path copc;
};

[[nodiscard]] PdalFixturePaths
writePdalFixtures(const std::filesystem::path &directory);
[[nodiscard]] std::filesystem::path
writePdalResidencyStressFixture(const std::filesystem::path &directory);

} // namespace pci::test
