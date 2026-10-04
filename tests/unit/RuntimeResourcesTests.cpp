#include <pci/runtime/HierarchyResidencyCoordinator.h>
#include <pci/runtime/PointMemoryBudget.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

namespace {

TEST_CASE("runtime resources share one explicit point-memory ceiling",
          "[unit][runtime-resources][memory]")
{
    auto memory = std::make_shared<pci::PointMemoryBudget>(1'000);
    auto retained = memory->tryReserve(200);
    REQUIRE(retained);

    auto coordinator = std::make_shared<pci::HierarchyResidencyCoordinator>(
        1'000, 1, pci::HierarchyDecodeAdmissionPtr{}, memory);
    const auto first = coordinator->registerParticipant(100);
    const auto second = coordinator->registerParticipant(100);

    CHECK(first->byteBudget() == 400);
    CHECK(second->byteBudget() == 400);
    CHECK(memory->reservedBytes() + first->byteBudget() +
              second->byteBudget() ==
          memory->byteBudget());

    retained.reset();
    CHECK(first->byteBudget() == 500);
    CHECK(second->byteBudget() == 500);
}

} // namespace
