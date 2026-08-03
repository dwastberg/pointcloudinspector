#include "pointcloud/PointColorMapCatalog.h"
#include "pointcloud/PointColorPolicy.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

TEST_CASE("color sources reflect available point-cloud attributes",
          "[unit][pointcloud]")
{
    pci::PointCloudMetadata metadata;
    metadata.hasColor = true;
    metadata.hasIntensity = true;
    metadata.hasClassification = true;
    metadata.hasReturnNumber = true;
    metadata.hasNumberOfReturns = true;

    CHECK(pci::availablePointColorSources(metadata) ==
          std::vector{
              pci::PointColorSource::Rgb,
              pci::PointColorSource::X,
              pci::PointColorSource::Y,
              pci::PointColorSource::Z,
              pci::PointColorSource::Intensity,
              pci::PointColorSource::Classification,
              pci::PointColorSource::ReturnNumber,
              pci::PointColorSource::NumberOfReturns,
          });

    metadata.hasColor = false;
    CHECK(pci::defaultPointColorMode(metadata) ==
          pci::PointColorMode{
              .source = pci::PointColorSource::Z,
              .colorMap = pci::PointColorMap::Viridis,
          });
}

TEST_CASE("color map policy only permits compatible maps", "[unit][pointcloud]")
{
    const auto catalog = pci::createBuiltInPointColorMapCatalog();
    CHECK(pci::availablePointColorMaps(*catalog, pci::PointColorSource::Rgb) ==
          std::vector{pci::PointColorMap::Rgb});
    CHECK(pci::availablePointColorMaps(*catalog, pci::PointColorSource::Z) ==
          std::vector{
              pci::PointColorMap::Grayscale,
              pci::PointColorMap::Viridis,
              pci::PointColorMap::Turbo,
          });
    CHECK(pci::availablePointColorMaps(*catalog,
                                       pci::PointColorSource::Classification) ==
          std::vector{pci::PointColorMap::LasClassification});
    CHECK(pci::availablePointColorMaps(*catalog,
                                       pci::PointColorSource::ReturnNumber) ==
          std::vector{pci::PointColorMap::ReturnNumber});

    const auto scalarMaps = pci::availablePointColorMaps(
        *catalog, pci::PointColorSource::Intensity);
    CHECK(pci::pointColorMapAvailable(scalarMaps, pci::PointColorMap::Viridis));
    CHECK_FALSE(pci::pointColorMapAvailable(
        scalarMaps, pci::PointColorMap::LasClassification));
}

TEST_CASE("color modes validate maps and manual scalar ranges",
          "[unit][pointcloud][color]")
{
    pci::PointCloudMetadata metadata;
    metadata.hasColor = true;
    metadata.hasIntensity = true;

    const auto catalog = pci::createBuiltInPointColorMapCatalog();
    CHECK(pci::pointColorModeAvailable(
        *catalog,
        metadata,
        {
            .source = pci::PointColorSource::Z,
            .colorMap = pci::PointColorMap::Turbo,
            .manualRange = pci::PointScalarRange{10.0, 20.0},
        }));
    CHECK_FALSE(pci::pointColorModeAvailable(
        *catalog,
        metadata,
        {
            .source = pci::PointColorSource::Z,
            .colorMap = pci::PointColorMap::LasClassification,
        }));
    CHECK_FALSE(pci::pointColorModeAvailable(
        *catalog,
        metadata,
        {
            .source = pci::PointColorSource::Intensity,
            .colorMap = pci::PointColorMap::Viridis,
            .manualRange = pci::PointScalarRange{20.0, 20.0},
        }));
    CHECK_FALSE(pci::pointColorModeAvailable(
        *catalog,
        metadata,
        {
            .source = pci::PointColorSource::Rgb,
            .colorMap = pci::PointColorMap::Rgb,
            .manualRange = pci::PointScalarRange{0.0, 1.0},
        }));
}

TEST_CASE("automatic coordinate ranges use document-wide bounds",
          "[unit][pointcloud][color][multi-layer]")
{
    const pci::Bounds3d documentBounds{
        .minimum = {-50.0, 200.0, 7.0},
        .maximum = {450.0, 900.0, 12.0},
    };
    const pci::PointCloudScalarRanges layerRanges{
        .intensity = pci::PointScalarRange{125.0, 800.0},
    };

    CHECK(pci::automaticPointColorRange(
              pci::PointColorSource::X, documentBounds, layerRanges) ==
          pci::PointScalarRange{-50.0, 450.0});
    CHECK(pci::automaticPointColorRange(
              pci::PointColorSource::Y, documentBounds, layerRanges) ==
          pci::PointScalarRange{200.0, 900.0});
    CHECK(pci::automaticPointColorRange(
              pci::PointColorSource::Z, documentBounds, layerRanges) ==
          pci::PointScalarRange{7.0, 12.0});
    CHECK(pci::automaticPointColorRange(
              pci::PointColorSource::Intensity, documentBounds, layerRanges) ==
          pci::PointScalarRange{125.0, 800.0});
    CHECK(pci::automaticPointColorRange(
              pci::PointColorSource::Intensity, documentBounds, {}) ==
          pci::PointScalarRange{0.0, 65535.0});

    const pci::PointColorMode manual{
        .source = pci::PointColorSource::Z,
        .colorMap = pci::PointColorMap::Viridis,
        .manualRange = pci::PointScalarRange{8.0, 9.0},
    };
    CHECK(pci::effectivePointColorRange(manual, documentBounds, layerRanges) ==
          manual.manualRange);
}

TEST_CASE("color map catalog owns interpolation and categorical palettes",
          "[unit][pointcloud][color]")
{
    const auto snapshot = pci::createBuiltInPointColorMapCatalog();
    const auto catalog = pci::pointColorMapCatalog(*snapshot);
    CHECK(catalog.size() == 6);
    for (const pci::PointColorMapDefinition &definition : catalog) {
        CHECK(pci::pointColorMapDefinition(*snapshot, definition.id) ==
              &definition);
        CHECK_FALSE(definition.key.empty());
        CHECK_FALSE(definition.name.empty());
    }

    const pci::PointRgba grayscaleMid = pci::sampleContinuousPointColorMap(
        *snapshot, pci::PointColorMap::Grayscale, 0.5F);
    CHECK(grayscaleMid.red == Catch::Approx(0.5F));
    CHECK(grayscaleMid.green == Catch::Approx(0.5F));
    CHECK(grayscaleMid.blue == Catch::Approx(0.5F));

    const pci::PointRgba ground = pci::sampleCategoricalPointColorMap(
        *snapshot, pci::PointColorMap::LasClassification, 2);
    CHECK(ground.red == Catch::Approx(0.45F));
    CHECK(ground.green == Catch::Approx(0.30F));
    CHECK(ground.blue == Catch::Approx(0.18F));
    const pci::PointRgba fallback = pci::sampleCategoricalPointColorMap(
        *snapshot, pci::PointColorMap::LasClassification, 255);
    CHECK(fallback.red == Catch::Approx(0.95F));
}

TEST_CASE("color map catalogs freeze independently with typed errors",
          "[unit][pointcloud][color]")
{
    pci::PointColorMapCatalog first;
    pci::PointColorMapCatalog second;
    const std::vector<pci::PointColorStop> stops{
        {0.0F, {0.0F, 0.0F, 0.0F, 1.0F}},
        {1.0F, {1.0F, 1.0F, 1.0F, 1.0F}},
    };
    const auto registration =
        first.registerContinuous("test:isolated", "Isolated", stops);
    REQUIRE(registration);
    const auto firstSnapshot = first.freeze();
    const auto secondSnapshot = second.freeze();
    CHECK(firstSnapshot->definitions().size() == 7);
    CHECK(secondSnapshot->definitions().size() == 6);
    CHECK(firstSnapshot->definition(registration.id) != nullptr);
    CHECK(secondSnapshot->definition(registration.id) == nullptr);

    const auto late = first.registerContinuous("test:late", "Late", stops);
    CHECK_FALSE(late);
    CHECK(late.errorCode ==
          pci::PointColorMapRegistrationResult::Error::Frozen);
}

} // namespace
