#include <pci/desktop/ui/SettingsDialog.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSpinBox>

TEST_CASE("settings dialog exposes appearance and depth controls",
          "[ui][settings]")
{
    pci::ViewportSettings initial;
    initial.backgroundColor = {.red = 0.25F, .green = 0.5F, .blue = 0.75F};
    initial.depthEnhancement = {
        .enabled = true,
        .radius = 2.0F,
        .strength = 35.0F,
    };
    const pci::PerformanceSettings performance{
        .automaticCpuCache = false,
        .cpuCacheMebibytes = 768,
        .gpuCacheMebibytes = 384,
        .maximumLoadPoints = 25'000'000,
    };
    pci::SettingsDialog dialog(initial, performance);
    CHECK_FALSE(dialog.isModal());
    CHECK(dialog.windowFlags().testFlag(Qt::Tool));

    int liveChangeCount = 0;
    QObject::connect(&dialog,
                     &pci::SettingsDialog::settingsChanged,
                     [&liveChangeCount](const pci::ViewportSettings &,
                                        const pci::PerformanceSettings &) {
                         ++liveChangeCount;
                     });

    auto *background = dialog.findChild<QPushButton *>(
        QStringLiteral("backgroundColorButton"));
    auto *enabled = dialog.findChild<QCheckBox *>(
        QStringLiteral("depthEnhancementEnabledCheckBox"));
    auto *radius = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("depthEnhancementRadiusSpinBox"));
    auto *strength = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("depthEnhancementStrengthSpinBox"));
    auto *restoreDefaults = dialog.findChild<QPushButton *>(
        QStringLiteral("restoreDefaultsButton"));
    auto *automaticCpu = dialog.findChild<QCheckBox *>(
        QStringLiteral("automaticCpuCacheCheckBox"));
    auto *cpuCache =
        dialog.findChild<QSpinBox *>(QStringLiteral("cpuCacheSpinBox"));
    auto *gpuCache =
        dialog.findChild<QSpinBox *>(QStringLiteral("gpuCacheSpinBox"));
    auto *maximumPoints = dialog.findChild<QDoubleSpinBox *>(
        QStringLiteral("maximumLoadPointsSpinBox"));
    REQUIRE(background != nullptr);
    REQUIRE(enabled != nullptr);
    REQUIRE(radius != nullptr);
    REQUIRE(strength != nullptr);
    REQUIRE(restoreDefaults != nullptr);
    REQUIRE(automaticCpu != nullptr);
    REQUIRE(cpuCache != nullptr);
    REQUIRE(gpuCache != nullptr);
    REQUIRE(maximumPoints != nullptr);

    CHECK(background->text() == QStringLiteral("#4080BF"));
    CHECK(enabled->isChecked());
    CHECK(radius->value() == Catch::Approx(2.0));
    CHECK(strength->value() == Catch::Approx(35.0));
    CHECK_FALSE(automaticCpu->isChecked());
    CHECK(cpuCache->isEnabled());
    CHECK(cpuCache->value() == 768);
    CHECK(gpuCache->value() == 384);
    CHECK(maximumPoints->value() == Catch::Approx(25'000'000.0));
    CHECK(maximumPoints->toolTip().contains(QStringLiteral("non-paged"),
                                            Qt::CaseInsensitive));

    strength->setValue(45.0);
    CHECK(liveChangeCount == 1);
    CHECK(dialog.settings().depthEnhancement.strength == 45.0F);

    background->click();
    auto *colorDialog = dialog.findChild<QColorDialog *>(
        QStringLiteral("backgroundColorDialog"));
    REQUIRE(colorDialog != nullptr);
    CHECK_FALSE(colorDialog->isModal());
    colorDialog->setCurrentColor(QColor(QStringLiteral("#CC4422")));
    CHECK(liveChangeCount == 2);
    CHECK(dialog.settings().backgroundColor.red ==
          Catch::Approx(QColor(QStringLiteral("#CC4422")).redF()));
    colorDialog->accept();

    automaticCpu->setChecked(true);
    CHECK(liveChangeCount == 3);
    CHECK_FALSE(cpuCache->isEnabled());

    enabled->setChecked(false);
    CHECK_FALSE(radius->isEnabled());
    CHECK_FALSE(strength->isEnabled());

    restoreDefaults->click();
    const pci::ViewportSettings restored = dialog.settings();
    const pci::ViewportSettings defaults;
    CHECK(restored.backgroundColor.red ==
          Catch::Approx(defaults.backgroundColor.red).margin(0.0001F));
    CHECK(restored.backgroundColor.green ==
          Catch::Approx(defaults.backgroundColor.green).margin(0.0001F));
    CHECK(restored.backgroundColor.blue ==
          Catch::Approx(defaults.backgroundColor.blue).margin(0.0001F));
    CHECK(restored.depthEnhancement == defaults.depthEnhancement);
    CHECK(dialog.performanceSettings() == pci::PerformanceSettings{});
    CHECK(radius->isEnabled());
    CHECK(strength->isEnabled());
}

TEST_CASE("settings dialog edits the raster budgets", "[ui][settings][raster]")
{
    const pci::PerformanceSettings performance{
        .raster =
            {
                .cpuCacheMebibytes = 384,
                .gpuCacheMebibytes = 192,
                .gdalCacheMebibytes = 64,
                .readWorkers = 3,
            },
    };
    pci::SettingsDialog dialog(pci::ViewportSettings{}, performance);

    auto *rasterCpu =
        dialog.findChild<QSpinBox *>(QStringLiteral("rasterCpuCacheSpinBox"));
    auto *rasterGpu =
        dialog.findChild<QSpinBox *>(QStringLiteral("rasterGpuCacheSpinBox"));
    auto *gdalCache =
        dialog.findChild<QSpinBox *>(QStringLiteral("gdalCacheSpinBox"));
    auto *workers =
        dialog.findChild<QSpinBox *>(QStringLiteral("rasterWorkersSpinBox"));
    REQUIRE(rasterCpu != nullptr);
    REQUIRE(rasterGpu != nullptr);
    REQUIRE(gdalCache != nullptr);
    REQUIRE(workers != nullptr);

    CHECK(rasterCpu->value() == 384);
    CHECK(rasterGpu->value() == 192);
    CHECK(gdalCache->value() == 64);
    CHECK(workers->value() == 3);
    // The read pool is built once, so the control says when the change lands
    // rather than appearing to take effect immediately.
    CHECK(workers->toolTip().contains(QStringLiteral("after restart")));

    int liveChangeCount = 0;
    QObject::connect(&dialog,
                     &pci::SettingsDialog::settingsChanged,
                     [&liveChangeCount](const pci::ViewportSettings &,
                                        const pci::PerformanceSettings &) {
                         ++liveChangeCount;
                     });

    rasterCpu->setValue(512);
    CHECK(liveChangeCount == 1);
    CHECK(dialog.performanceSettings().raster.cpuCacheMebibytes == 512);

    // A budget below the working minimum is raised rather than accepted: the
    // spin box floor and the clamp must agree, or an apply would silently
    // differ from what the dialog shows.
    rasterGpu->setValue(1);
    CHECK(rasterGpu->value() ==
          static_cast<int>(pci::minimumRasterGpuCacheMebibytes));
    CHECK(dialog.performanceSettings().raster.gpuCacheMebibytes ==
          pci::minimumRasterGpuCacheMebibytes);

    // Every other field must survive an edit to one of them.
    CHECK(dialog.performanceSettings().raster.gdalCacheMebibytes == 64);
    CHECK(dialog.performanceSettings().raster.readWorkers == 3);

    auto *restoreDefaults = dialog.findChild<QPushButton *>(
        QStringLiteral("restoreDefaultsButton"));
    REQUIRE(restoreDefaults != nullptr);
    restoreDefaults->click();
    CHECK(dialog.performanceSettings().raster ==
          pci::RasterPerformanceSettings{});
}
