#include "app/EmbeddedColorMaps.h"

#include "pointcloud/PointColorMapCatalog.h"
#include "pointcloud/PointColorPolicy.h"
#include "renderer/PointColorMapAtlas.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace {

TEST_CASE("embedded CPT resources join the startup color-map catalog",
          "[qt][color][cpt][resource]")
{
    pci::PointColorMapCatalog mutableCatalog;
    const pci::EmbeddedColorMapLoadResult loaded =
        pci::loadEmbeddedColorMaps(mutableCatalog);
    INFO("embedded CPT issue count: " << loaded.issues.size());
    for (const pci::EmbeddedColorMapIssue &issue : loaded.issues) {
        INFO(issue.resourcePath.toStdString()
             << ':' << issue.line << ": " << issue.message.toStdString());
    }
    REQUIRE(loaded.issues.empty());
    REQUIRE(loaded.loadedMapCount == 1);

    const auto snapshot = mutableCatalog.freeze();
    const std::span catalog = pci::pointColorMapCatalog(*snapshot);
    const auto embedded =
        std::ranges::find(catalog,
                          std::string_view{"cpt:embedded-test.cpt"},
                          &pci::PointColorMapDefinition::key);
    REQUIRE(embedded != catalog.end());
    CHECK(embedded->name == "Embedded test gradient");
    CHECK(pci::pointColorMapSupportsSource(
        *snapshot, embedded->id, pci::PointColorSource::Z));
    CHECK_FALSE(pci::pointColorMapSupportsSource(
        *snapshot, embedded->id, pci::PointColorSource::Classification));

    const pci::PointRgba low =
        pci::sampleContinuousPointColorMap(*snapshot, embedded->id, 0.0F);
    const pci::PointRgba high =
        pci::sampleContinuousPointColorMap(*snapshot, embedded->id, 1.0F);
    CHECK(low.red == Catch::Approx(32.0F / 255.0F));
    CHECK(low.green == Catch::Approx(64.0F / 255.0F));
    CHECK(low.blue == Catch::Approx(96.0F / 255.0F));
    CHECK(high.red == Catch::Approx(224.0F / 255.0F));
    CHECK(high.green == Catch::Approx(240.0F / 255.0F));
    CHECK(high.blue == Catch::Approx(1.0F));

    const QImage atlas = pci::buildPointColorMapAtlas(*snapshot);
    CHECK(atlas.height() ==
          static_cast<int>(catalog.size()) * pci::pointColorMapAtlasRowsPerMap);
    const pci::PointColorMapSampling sampling =
        pci::pointColorMapSampling(*snapshot, embedded->id);
    const int atlasRow = static_cast<int>(sampling.rowCoordinate *
                                          static_cast<float>(atlas.height()));
    CHECK(atlas.pixelColor(0, atlasRow).redF() ==
          Catch::Approx(32.0F / 255.0F).margin(0.005F));
    CHECK(atlas.pixelColor(atlas.width() - 1, atlasRow).redF() ==
          Catch::Approx(224.0F / 255.0F).margin(0.005F));

    const pci::PointColorMapRegistrationResult lateRegistration =
        mutableCatalog.registerContinuous("test:late",
                                          "Too late",
                                          {
                                              {0.0F, {0.0F, 0.0F, 0.0F, 1.0F}},
                                              {1.0F, {1.0F, 1.0F, 1.0F, 1.0F}},
                                          });
    CHECK_FALSE(lateRegistration);
    CHECK(lateRegistration.errorCode ==
          pci::PointColorMapRegistrationResult::Error::Frozen);
    CHECK_FALSE(lateRegistration.error.empty());
}

} // namespace
