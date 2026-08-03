#include "app/VectorColorButton.h"
#include <catch2/catch_test_macros.hpp>
TEST_CASE("vector color button normalizes RGBA values", "[ui][vector]")
{
    pci::VectorColorButton button;
    button.setColor({2.0F, -1.0F, 0.25F, 0.5F});
    CHECK(button.color() == pci::VectorRgba{1.0F, 0.0F, 0.25F, 0.5F});
    CHECK_FALSE(button.accessibleDescription().isEmpty());
}
