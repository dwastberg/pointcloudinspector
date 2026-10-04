#include <pci/desktop/ui/ClassificationFilterDialog.h>

#include <pci/color/PointColorMapCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSize>

#include <array>
#include <cstdint>

namespace {

QRgb swatchColor(const QListWidgetItem &item)
{
    // Request the icon at its own resolution so no scaling can tint the
    // sampled center pixel.
    const QSize size = item.icon().actualSize(QSize(64, 64));
    const QImage swatch = item.icon().pixmap(size).toImage();
    return swatch.pixel(swatch.width() / 2, swatch.height() / 2);
}

QRgb classificationMapColor(const std::uint8_t classification)
{
    const auto catalog = pci::createBuiltInPointColorMapCatalog();
    const pci::PointRgba color = pci::sampleCategoricalPointColorMap(
        *catalog, pci::PointColorMap::LasClassification, classification);
    return QColor::fromRgbF(color.red, color.green, color.blue, color.alpha)
        .rgb();
}

TEST_CASE("classification filter rows carry color-map swatches",
          "[ui][classification]")
{
    // 17 has no dedicated palette entry, so its swatch must show the
    // classification map's fallback color — what the viewport draws too.
    constexpr std::array<std::uint8_t, 4> present{2, 5, 6, 17};
    pci::PointClassificationFilter available =
        pci::PointClassificationFilter::noneVisible();
    for (const std::uint8_t classification : present) {
        available.setVisible(classification, true);
    }

    pci::ClassificationFilterDialog dialog(pci::PointClassificationFilter{},
                                           available,
                                           QStringLiteral("classified.las"));

    auto *list = dialog.findChild<QListWidget *>(
        QStringLiteral("classificationFilterList"));
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == static_cast<int>(present.size()));
    CHECK(list->iconSize().width() > 0);
    CHECK(list->iconSize().width() <= 16);

    for (int row = 0; row < list->count(); ++row) {
        const QListWidgetItem *item = list->item(row);
        const auto classification =
            static_cast<std::uint8_t>(item->data(Qt::UserRole).toInt());
        REQUIRE_FALSE(item->icon().isNull());
        CHECK(swatchColor(*item) == classificationMapColor(classification));
    }
}

} // namespace
