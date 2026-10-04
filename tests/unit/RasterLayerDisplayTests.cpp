#include <pci/raster/RasterLayerDisplay.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("raster decode parameters apply the edited scalar range and ramp",
          "[unit][raster][color]")
{
    pci::RasterLayerMetadata metadata;
    metadata.defaultDisplay.sampleKind =
        pci::RasterSampleKind::ContinuousScalar;
    metadata.defaultDisplay.displayRange =
        pci::RasterDisplayRange{.minimum = 0.0, .maximum = 100.0};

    pci::RasterLayerStyle style;
    style.displayRange =
        pci::RasterDisplayRange{.minimum = 20.0, .maximum = 40.0};
    style.colorRampKey = std::string(pci::defaultRasterScalarColorRampKey);

    pci::PointColorMapCatalog catalog;
    const auto registration = catalog.registerContinuous(
        std::string(pci::defaultRasterScalarColorRampKey),
        "Test viridis",
        {{.position = 0.0F, .color = {1.0F, 0.0F, 0.0F, 1.0F}},
         {.position = 1.0F, .color = {0.0F, 1.0F, 0.0F, 1.0F}}});
    REQUIRE(registration);

    const auto decode =
        pci::resolveRasterDecodeParameters(metadata, style, *catalog.freeze());
    REQUIRE(decode != nullptr);
    REQUIRE(decode->displayRange.has_value());
    CHECK(decode->displayRange->minimum == 20.0);
    CHECK(decode->displayRange->maximum == 40.0);
    REQUIRE(decode->colorRamp != nullptr);
    CHECK(decode->colorRamp->size() == 2);
}
