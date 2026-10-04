#pragma once

#include "ToolbarIcons.h"
#include <pci/adapters/platform/QtPath.h>
#include <pci/desktop/ui/ClassificationFilterDialog.h>
#include <pci/desktop/ui/RasterColorizeUiState.h>
#include <pci/desktop/ui/VectorColorButton.h>
#include <pci/desktop/viewport/PointColorLabels.h>

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pci::inspector {

inline void addPointColorMapOption(QComboBox &combo,
                                   const PointColorMapCatalogSnapshot &catalog,
                                   const PointColorMap map)
{
    combo.addItem(pointColorMapLabel(catalog, map), static_cast<int>(map));
    combo.setItemData(
        combo.count() - 1, pointColorMapToolTip(catalog, map), Qt::ToolTipRole);
}

inline void updatePointColorMapToolTip(QComboBox &combo)
{
    combo.setToolTip(combo.currentData(Qt::ToolTipRole).toString());
}

inline QString layerLabel(const PointCloudLayerSnapshot &layer)
{
    const std::filesystem::path &path = layer.descriptor.metadata.sourcePath;
    return path.empty() ? QStringLiteral("Point cloud %1").arg(layer.id.value())
                        : displayPathName(path);
}

inline QString layerLabel(const VectorLayerSnapshot &layer)
{
    if (!layer.data) {
        return QStringLiteral("Vector layer %1").arg(layer.id.value());
    }
    if (!layer.data->sublayerName.empty()) {
        return QString::fromStdString(layer.data->sublayerName);
    }
    return layer.data->sourcePath.filename().empty()
               ? QStringLiteral("Vector layer %1").arg(layer.id.value())
               : displayPathName(layer.data->sourcePath);
}

inline QString attributesText(const PointCloudMetadata &metadata)
{
    QStringList parts;
    if (metadata.hasColor) {
        parts << QStringLiteral("RGB");
    }
    if (metadata.hasIntensity) {
        parts << QStringLiteral("Intensity");
    }
    if (metadata.hasClassification) {
        parts << QStringLiteral("Classification");
    }
    if (metadata.hasReturnNumber) {
        parts << QStringLiteral("Return #");
    }
    if (metadata.hasNumberOfReturns) {
        parts << QStringLiteral("Returns");
    }
    return parts.isEmpty() ? QStringLiteral("—")
                           : parts.join(QStringLiteral(", "));
}

inline QString boundsText(const Bounds3d &bounds)
{
    if (!bounds.valid()) {
        return QStringLiteral("—");
    }
    const double dx = bounds.maximum[0] - bounds.minimum[0];
    const double dy = bounds.maximum[1] - bounds.minimum[1];
    const double dz = bounds.maximum[2] - bounds.minimum[2];
    return QStringLiteral("%1 × %2 × %3")
        .arg(dx, 0, 'f', 2)
        .arg(dy, 0, 'f', 2)
        .arg(dz, 0, 'f', 2);
}

inline QString rasterDecodeText(const RasterDecodeParameters &decode)
{
    switch (decode.sampleKind) {
    case RasterSampleKind::ContinuousColor:
        return QStringLiteral("RGB display");
    case RasterSampleKind::Categorical:
        return QStringLiteral("palette display");
    case RasterSampleKind::ContinuousScalar:
        return decode.displayRange
                   ? QStringLiteral("scalar range %1 to %2 with color ramp")
                         .arg(decode.displayRange->minimum, 0, 'g', 8)
                         .arg(decode.displayRange->maximum, 0, 'g', 8)
                   : QStringLiteral("scalar color ramp");
    }
    return QStringLiteral("display colors");
}

// A single-line value label that holds its full text (kept as a tooltip and
// for elision) but whose size hint is a small, *text-independent* width, so
// long metadata (source paths, CRS WKT) can never grow the dock. It expands to
// fill the column it is given, and middle-elides to that width at paint time,
// so it always fills — and never overflows — whatever the layout assigns.
class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        const QFontMetrics metrics(font());
        return {metrics.averageCharWidth() * 8, metrics.height()};
    }

    [[nodiscard]] QSize minimumSizeHint() const override
    {
        const QFontMetrics metrics(font());
        return {metrics.averageCharWidth() * 4, metrics.height()};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        const QFontMetrics metrics(font());
        const QString elided =
            metrics.elidedText(text(), Qt::ElideMiddle, contentsRect().width());
        painter.drawText(contentsRect(), static_cast<int>(alignment()), elided);
    }
};

inline QLabel *makeValueLabel()
{
    auto *label = new ElidedLabel();
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setText(QStringLiteral("—"));
    return label;
}

inline void setValueText(QLabel *label, const QString &text)
{
    label->setText(text);
    label->setToolTip(text);
}

inline std::size_t
visibleClassificationCount(const PointClassificationFilter &filter,
                           const PointClassificationFilter &available)
{
    std::size_t result = 0;
    for (std::size_t classification = 0;
         classification < pointClassificationCount;
         ++classification) {
        const auto value = static_cast<std::uint8_t>(classification);
        result +=
            filter.isVisible(value) && available.isVisible(value) ? 1U : 0U;
    }
    return result;
}

} // namespace pci::inspector
