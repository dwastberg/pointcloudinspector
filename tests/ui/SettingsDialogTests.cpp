#include "app/SettingsDialog.h"

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
