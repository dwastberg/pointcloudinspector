#include "app/VectorSublayerDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("vector sublayer dialog returns checked layers", "[ui][vector]")
{
    pci::VectorImportPreflight preflight;
    preflight.sourcePath = "survey.gpkg";
    preflight.driverName = "GPKG";
    preflight.sublayers = {
        {.key = {.index = 3, .name = "roads"},
         .geometryTypeLabel = "LineString",
         .featureCount = 12},
        {.key = {.index = 4, .name = "parcels"},
         .geometryTypeLabel = "Polygon",
         .featureCount = 250},
    };
    pci::VectorSublayerDialog dialog(std::move(preflight));
    auto *list =
        dialog.findChild<QListWidget *>(QStringLiteral("vectorSublayerList"));
    auto *summary =
        dialog.findChild<QLabel *>(QStringLiteral("vectorSublayerSummary"));
    auto *source =
        dialog.findChild<QLabel *>(QStringLiteral("vectorImportSource"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>(
        QStringLiteral("vectorSublayerButtons"));
    REQUIRE(list);
    REQUIRE(summary);
    REQUIRE(source);
    REQUIRE(buttons);
    CHECK(source->text().contains(QStringLiteral("survey.gpkg")));
    CHECK(source->text().contains(QStringLiteral("GPKG")));
    CHECK(list->item(0)->text().contains(QStringLiteral("12 features")));
    CHECK(summary->text() == QStringLiteral("2 of 2 selected"));
    CHECK(buttons->button(QDialogButtonBox::Ok)->text() ==
          QStringLiteral("Add 2 layers"));

    list->item(1)->setCheckState(Qt::Unchecked);
    const auto selected = dialog.selectedSublayers();
    REQUIRE(selected.size() == 1);
    CHECK(selected.front().name == "roads");
    CHECK(summary->text() == QStringLiteral("1 of 2 selected"));
    CHECK(buttons->button(QDialogButtonBox::Ok)->text() ==
          QStringLiteral("Add layer"));
}

TEST_CASE("vector sublayer dialog bulk selection prevents an empty import",
          "[ui][vector]")
{
    pci::VectorImportPreflight preflight;
    preflight.sublayers = {
        {.key = {.index = 0, .name = "points"}, .geometryTypeLabel = "Point"},
        {.key = {.index = 1, .name = "lines"},
         .geometryTypeLabel = "LineString"},
    };
    pci::VectorSublayerDialog dialog(std::move(preflight));
    auto *clear = dialog.findChild<QPushButton *>(
        QStringLiteral("clearVectorSublayersButton"));
    auto *selectAll = dialog.findChild<QPushButton *>(
        QStringLiteral("selectAllVectorSublayersButton"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>(
        QStringLiteral("vectorSublayerButtons"));
    REQUIRE(clear);
    REQUIRE(selectAll);
    REQUIRE(buttons);
    auto *list =
        dialog.findChild<QListWidget *>(QStringLiteral("vectorSublayerList"));
    REQUIRE(list);
    CHECK(list->item(0)->text().contains(
        QStringLiteral("Feature count unavailable")));

    clear->click();
    CHECK(dialog.selectedSublayers().empty());
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(buttons->button(QDialogButtonBox::Ok)->text() ==
          QStringLiteral("Add 0 layers"));

    selectAll->click();
    CHECK(dialog.selectedSublayers().size() == 2);
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
}

TEST_CASE("vector sublayer dialog opens without a nested event loop",
          "[ui][vector]")
{
    pci::VectorImportPreflight preflight;
    preflight.sublayers = {
        {.key = {.index = 0, .name = "parcels"},
         .geometryTypeLabel = "Polygon"},
        {.key = {.index = 1, .name = "roads"},
         .geometryTypeLabel = "LineString"},
    };
    pci::VectorSublayerDialog dialog(std::move(preflight));
    QSignalSpy finished(&dialog, &QDialog::finished);

    dialog.open();
    CHECK(dialog.isVisible());
    QTimer::singleShot(0, &dialog, &QDialog::reject);
    REQUIRE(finished.wait(1000));
    CHECK(finished.at(0).at(0).toInt() == QDialog::Rejected);
}
