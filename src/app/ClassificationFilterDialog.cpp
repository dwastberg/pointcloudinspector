#include "app/ClassificationFilterDialog.h"

#include "pointcloud/PointColorMapCatalog.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QSignalBlocker>
#include <QSize>
#include <QVBoxLayout>

#include <cstdint>
#include <utility>

namespace pci {
namespace {

constexpr int classificationSwatchSize = 12;

// Swatches read from the same palette that "Color by classification" renders
// with, so the dialog and the viewport can never disagree about a class color.
QColor classificationColor(const PointColorMapCatalogSnapshot &catalog,
                           const int classification)
{
    const PointRgba color = sampleCategoricalPointColorMap(
        catalog,
        PointColorMap::LasClassification,
        static_cast<std::uint8_t>(classification));
    return QColor::fromRgbF(color.red, color.green, color.blue, color.alpha);
}

QIcon classificationSwatch(const QColor &fill, const QColor &rim)
{
    // Drawn at twice the on-screen size: an exact source for high-DPI screens
    // and a clean one to scale down elsewhere.
    constexpr int scale = 2;
    constexpr int extent = classificationSwatchSize * scale;
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(rim, scale));
    painter.setBrush(fill);
    constexpr qreal inset = static_cast<qreal>(scale) / 2.0;
    painter.drawRoundedRect(
        QRectF(inset, inset, extent - scale, extent - scale),
        3.0 * scale,
        3.0 * scale);
    return QIcon(pixmap);
}

QString classificationName(const int classification)
{
    switch (classification) {
    case 0:
        return QStringLiteral("Created, never classified");
    case 1:
        return QStringLiteral("Unclassified");
    case 2:
        return QStringLiteral("Ground");
    case 3:
        return QStringLiteral("Low vegetation");
    case 4:
        return QStringLiteral("Medium vegetation");
    case 5:
        return QStringLiteral("High vegetation");
    case 6:
        return QStringLiteral("Building");
    case 7:
        return QStringLiteral("Low point (noise)");
    case 8:
        return QStringLiteral("Reserved");
    case 9:
        return QStringLiteral("Water");
    case 10:
        return QStringLiteral("Rail");
    case 11:
        return QStringLiteral("Road surface");
    case 12:
        return QStringLiteral("Reserved");
    case 13:
        return QStringLiteral("Wire — guard");
    case 14:
        return QStringLiteral("Wire — conductor");
    case 15:
        return QStringLiteral("Transmission tower");
    case 16:
        return QStringLiteral("Wire-structure connector");
    case 17:
        return QStringLiteral("Bridge deck");
    case 18:
        return QStringLiteral("High noise");
    case 19:
        return QStringLiteral("Overhead structure");
    case 20:
        return QStringLiteral("Ignored ground");
    case 21:
        return QStringLiteral("Snow");
    case 22:
        return QStringLiteral("Temporal exclusion");
    default:
        return classification < 64 ? QStringLiteral("Reserved")
                                   : QStringLiteral("User-defined");
    }
}

} // namespace

ClassificationFilterDialog::ClassificationFilterDialog(
    PointClassificationFilter filter,
    PointClassificationFilter availableClassifications,
    const QString &layerName,
    QWidget *parent)
    : ClassificationFilterDialog(createBuiltInPointColorMapCatalog(),
                                 filter,
                                 availableClassifications,
                                 layerName,
                                 parent)
{
}

ClassificationFilterDialog::ClassificationFilterDialog(
    PointColorMapCatalogSnapshotPtr colorMaps,
    const PointClassificationFilter filter,
    const PointClassificationFilter availableClassifications,
    const QString &layerName,
    QWidget *parent)
    : QDialog(parent)
    , baseFilter_(filter)
    , colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
{
    setObjectName(QStringLiteral("classificationFilterDialog"));
    setWindowTitle(QStringLiteral("Filter classifications"));
    setModal(true);
    setMinimumSize(420, 560);

    auto *layout = new QVBoxLayout(this);
    auto *description =
        new QLabel(QStringLiteral("Choose the classifications to show in %1.")
                       .arg(layerName),
                   this);
    description->setWordWrap(true);
    layout->addWidget(description);

    classificationList_ = new QListWidget(this);
    classificationList_->setObjectName(
        QStringLiteral("classificationFilterList"));
    classificationList_->setIconSize(
        QSize(classificationSwatchSize, classificationSwatchSize));
    QColor swatchRim = palette().color(QPalette::Text);
    swatchRim.setAlphaF(0.3F);
    for (int classification = 0;
         classification < static_cast<int>(pointClassificationCount);
         ++classification) {
        if (!availableClassifications.isVisible(
                static_cast<std::uint8_t>(classification))) {
            continue;
        }
        auto *item =
            new QListWidgetItem(QStringLiteral("%1 — %2")
                                    .arg(classification)
                                    .arg(classificationName(classification)),
                                classificationList_);
        item->setIcon(classificationSwatch(
            classificationColor(*colorMaps_, classification), swatchRim));
        item->setData(Qt::UserRole, classification);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(
            filter.isVisible(static_cast<std::uint8_t>(classification))
                ? Qt::Checked
                : Qt::Unchecked);
    }
    layout->addWidget(classificationList_, 1);

    auto *selectionLayout = new QHBoxLayout();
    auto *selectAll = new QPushButton(QStringLiteral("Select all"), this);
    selectAll->setObjectName(QStringLiteral("selectAllClassificationsButton"));
    auto *clearAll = new QPushButton(QStringLiteral("Clear all"), this);
    clearAll->setObjectName(QStringLiteral("clearAllClassificationsButton"));
    selectionLayout->addWidget(selectAll);
    selectionLayout->addWidget(clearAll);
    selectionLayout->addStretch(1);
    layout->addLayout(selectionLayout);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(QStringLiteral("classificationFilterSummary"));
    layout->addWidget(summaryLabel_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("classificationFilterButtonBox"));
    auto *applySelected =
        buttons->addButton(QStringLiteral("Apply to selected layer"),
                           QDialogButtonBox::ActionRole);
    applySelected->setObjectName(
        QStringLiteral("applyClassificationToSelectedButton"));
    applySelected->setDefault(true);
    auto *applyAll = buttons->addButton(QStringLiteral("Apply to all layers"),
                                        QDialogButtonBox::ActionRole);
    applyAll->setObjectName(QStringLiteral("applyClassificationToAllButton"));
    layout->addWidget(buttons);

    connect(selectAll, &QPushButton::clicked, this, [this] {
        setAllChecked(true);
    });
    connect(clearAll, &QPushButton::clicked, this, [this] {
        setAllChecked(false);
    });
    connect(classificationList_,
            &QListWidget::itemChanged,
            this,
            [this](QListWidgetItem *) {
                updateSummary();
            });
    connect(applySelected, &QPushButton::clicked, this, [this] {
        applyToAllLayers_ = false;
        accept();
    });
    connect(applyAll, &QPushButton::clicked, this, [this] {
        applyToAllLayers_ = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateSummary();
}

PointClassificationFilter ClassificationFilterDialog::filter() const
{
    PointClassificationFilter result = baseFilter_;
    for (int row = 0; row < classificationList_->count(); ++row) {
        const QListWidgetItem *item = classificationList_->item(row);
        result.setVisible(
            static_cast<std::uint8_t>(item->data(Qt::UserRole).toInt()),
            item->checkState() == Qt::Checked);
    }
    return result;
}

bool ClassificationFilterDialog::applyToAllLayers() const noexcept
{
    return applyToAllLayers_;
}

void ClassificationFilterDialog::setAllChecked(const bool checked)
{
    const QSignalBlocker blocker(classificationList_);
    for (int row = 0; row < classificationList_->count(); ++row) {
        classificationList_->item(row)->setCheckState(checked ? Qt::Checked
                                                              : Qt::Unchecked);
    }
    updateSummary();
}

void ClassificationFilterDialog::updateSummary()
{
    std::size_t visible = 0;
    for (int row = 0; row < classificationList_->count(); ++row) {
        visible += classificationList_->item(row)->checkState() == Qt::Checked
                       ? 1U
                       : 0U;
    }
    const std::size_t available =
        static_cast<std::size_t>(classificationList_->count());
    if (available == 0) {
        summaryLabel_->setText(
            QStringLiteral("No classifications have been observed yet."));
    } else if (visible == available) {
        summaryLabel_->setText(
            QStringLiteral("All available classifications are visible."));
    } else if (visible == 0) {
        summaryLabel_->setText(QStringLiteral(
            "No classifications are selected; this cloud will be hidden."));
    } else {
        summaryLabel_->setText(
            QStringLiteral("%1 of %2 classifications are visible.")
                .arg(visible)
                .arg(available));
    }
}

} // namespace pci
