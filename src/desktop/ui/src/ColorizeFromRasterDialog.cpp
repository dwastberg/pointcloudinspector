#include <pci/desktop/ui/ColorizeFromRasterDialog.h>

#include <pci/adapters/platform/QtPath.h>
#include <pci/desktop/ui/SceneLayerMetaType.h>
#include <pci/foundation/CheckedArithmetic.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <filesystem>

namespace pci {
namespace {

[[nodiscard]] bool xyDisjoint(const Bounds3d &left,
                              const Bounds3d &right) noexcept
{
    return left.maximum[0] < right.minimum[0] ||
           right.maximum[0] < left.minimum[0] ||
           left.maximum[1] < right.minimum[1] ||
           right.maximum[1] < left.minimum[1];
}

[[nodiscard]] QString shortCrs(const std::string &wkt)
{
    if (wkt.empty()) {
        return QStringLiteral("missing");
    }
    QString result = QString::fromStdString(wkt).simplified();
    constexpr qsizetype maximumCharacters = 96;
    if (result.size() > maximumCharacters) {
        result = result.left(maximumCharacters - 1) + QChar(0x2026);
    }
    return result;
}

[[nodiscard]] QString displayTransformSummary(const RasterLayerSnapshot &raster)
{
    RasterDecodeParameters display = raster.descriptor.metadata.defaultDisplay;
    if (raster.style.displayRange) {
        display.displayRange = raster.style.displayRange;
    }
    switch (display.sampleKind) {
    case RasterSampleKind::ContinuousColor:
        return QStringLiteral("RGB display");
    case RasterSampleKind::Categorical:
        return QStringLiteral("palette display");
    case RasterSampleKind::ContinuousScalar:
        if (display.displayRange) {
            return QStringLiteral("scalar range %1 to %2%3")
                .arg(display.displayRange->minimum, 0, 'g', 8)
                .arg(display.displayRange->maximum, 0, 'g', 8)
                .arg(raster.style.colorRampKey.empty()
                         ? QStringLiteral(" with color ramp")
                         : QStringLiteral(" with %1")
                               .arg(QString::fromStdString(
                                   raster.style.colorRampKey)));
        }
        return QStringLiteral("scalar color ramp");
    }
    return QStringLiteral("display colors");
}

[[nodiscard]] QString mebibytes(const std::uint64_t bytes)
{
    return QStringLiteral("%1 MiB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

void setNoticeStyle(QLabel &label, const QString &severity)
{
    label.setProperty("notice", severity);
    label.style()->unpolish(&label);
    label.style()->polish(&label);
}

} // namespace

ColorizeFromRasterDialog::ColorizeFromRasterDialog(
    PointCloudLayerSnapshot target,
    SceneDocumentSnapshotPtr document,
    RasterColorizeEstimateFunction estimate,
    std::shared_ptr<const SpatialReferenceComparator> comparator,
    const std::uint64_t availablePointMemoryBytes,
    QWidget *parent)
    : QDialog(parent)
    , target_(std::move(target))
    , document_(std::move(document))
    , estimateFunction_(std::move(estimate))
    , comparator_(std::move(comparator))
    , availablePointMemoryBytes_(availablePointMemoryBytes)
{
    setObjectName(QStringLiteral("colorizeFromRasterDialog"));
    setWindowTitle(QStringLiteral("Colorize Point Cloud from Raster"));
    setModal(true);
    resize(560, 410);

    auto *layout = new QVBoxLayout(this);
    const QString targetName =
        target_.descriptor.metadata.sourcePath.empty()
            ? QStringLiteral("Point cloud %1").arg(target_.id.value())
            : displayPathName(target_.descriptor.metadata.sourcePath);
    auto *targetLabel =
        new QLabel(QStringLiteral("Transfer the raster's displayed colors to "
                                  "%1.")
                       .arg(targetName),
                   this);
    targetLabel->setObjectName(QStringLiteral("colorizeTargetLabel"));
    targetLabel->setWordWrap(true);
    layout->addWidget(targetLabel);

    auto *transferGroup = new QGroupBox(QStringLiteral("Color transfer"), this);
    auto *transferLayout = new QFormLayout(transferGroup);
    rasters_ = new QComboBox(transferGroup);
    rasters_->setObjectName(QStringLiteral("colorizeRasterCombo"));
    const RasterLayerSnapshotView rasterLayers = document_->rasterLayers();
    std::optional<int> topVisible;
    for (const RasterLayerSnapshot &raster : rasterLayers) {
        const QString name =
            pathToQString(raster.descriptor.metadata.sourcePath.filename());
        const QString label =
            name.isEmpty() ? QStringLiteral("Raster layer") : name;
        rasters_->addItem(QStringLiteral("%1 · %2").arg(
                              label,
                              raster.visible ? QStringLiteral("visible")
                                             : QStringLiteral("hidden")),
                          QVariant::fromValue(raster.id));
        // Later document rows are painted above earlier rows.
        if (raster.visible) {
            topVisible = rasters_->count() - 1;
        }
    }
    if (topVisible) {
        rasters_->setCurrentIndex(*topVisible);
    }
    transferLayout->addRow(QStringLiteral("Raster layer"), rasters_);

    summary_ = new QLabel(transferGroup);
    summary_->setObjectName(QStringLiteral("colorizeRasterSummary"));
    summary_->setWordWrap(true);
    transferLayout->addRow(QStringLiteral("Display"), summary_);
    layout->addWidget(transferGroup);

    auto *resourceGroup =
        new QGroupBox(QStringLiteral("Resource impact"), this);
    auto *resourceLayout = new QVBoxLayout(resourceGroup);
    estimate_ = new QLabel(resourceGroup);
    estimate_->setObjectName(QStringLiteral("colorizeRasterEstimate"));
    estimate_->setWordWrap(true);
    setNoticeStyle(*estimate_, QStringLiteral("info"));
    resourceLayout->addWidget(estimate_);
    layout->addWidget(resourceGroup);

    warning_ = new QLabel(this);
    warning_->setObjectName(QStringLiteral("colorizeRasterWarning"));
    warning_->setWordWrap(true);
    setNoticeStyle(*warning_, QStringLiteral("warning"));
    layout->addWidget(warning_);

    auto *invariant = new QLabel(
        QStringLiteral(
            "Only opaque raster pixels change points. Unmatched points keep "
            "their exact source color; Revert restores the original colors."),
        this);
    invariant->setObjectName(QStringLiteral("colorizeRasterBehaviorNotice"));
    invariant->setWordWrap(true);
    setNoticeStyle(*invariant, QStringLiteral("info"));
    layout->addWidget(invariant);
    if (target_.rasterColors) {
        auto *replacementNotice = new QLabel(
            QStringLiteral("The current colors remain visible and reserved "
                           "until this replacement is complete."),
            this);
        replacementNotice->setObjectName(
            QStringLiteral("colorizeRasterReplacementNotice"));
        replacementNotice->setWordWrap(true);
        setNoticeStyle(*replacementNotice, QStringLiteral("warning"));
        layout->addWidget(replacementNotice);
    }
    layout->addStretch(1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    apply_ = buttons_->addButton(target_.rasterColors
                                     ? QStringLiteral("Replace raster colors")
                                     : QStringLiteral("Colorize layer"),
                                 QDialogButtonBox::AcceptRole);
    apply_->setObjectName(QStringLiteral("colorizeRasterApplyButton"));
    apply_->setProperty("primary", true);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(rasters_, &QComboBox::currentIndexChanged, this, [this] {
        updateSelection();
    });
    layout->addWidget(buttons_);
    updateSelection();
}

std::optional<SceneLayerId>
ColorizeFromRasterDialog::selectedRasterLayerId() const
{
    if (rasters_->currentIndex() < 0) {
        return std::nullopt;
    }
    return rasters_->currentData().value<SceneLayerId>();
}

void ColorizeFromRasterDialog::updateSelection()
{
    const auto id = selectedRasterLayerId();
    const auto raster =
        id ? document_->rasterLayer(*id) : std::optional<RasterLayerSnapshot>{};
    if (!raster) {
        apply_->setEnabled(false);
        summary_->clear();
        warning_->setText(QStringLiteral("Import a raster layer first."));
        warning_->setVisible(true);
        setNoticeStyle(*warning_, QStringLiteral("error"));
        estimate_->clear();
        return;
    }

    const RasterLayerMetadata &metadata = raster->descriptor.metadata;
    summary_->setText(QStringLiteral("%1 × %2 pixels, %3 bands · %4")
                          .arg(metadata.width)
                          .arg(metadata.height)
                          .arg(metadata.bands.size())
                          .arg(displayTransformSummary(*raster)));

    bool admitted = true;
    try {
        const RasterColorizeResourceEstimate estimate =
            estimateFunction_(raster->id);
        const std::uint64_t requiredPointMemory = saturatingAdd(
            estimate.persistentColorBytes, estimate.workingMemoryBytes);
        estimate_->setText(
            QStringLiteral("Colors: %1 · Working memory: %2 · Temporary "
                           "disk: up to %3")
                .arg(mebibytes(estimate.persistentColorBytes),
                     mebibytes(estimate.workingMemoryBytes),
                     mebibytes(estimate.maximumTemporaryBytes)));
        if (requiredPointMemory > availablePointMemoryBytes_) {
            QString refusal =
                QStringLiteral("This operation needs %1 of additional "
                               "point memory, but only %2 is available.")
                    .arg(mebibytes(requiredPointMemory),
                         mebibytes(availablePointMemoryBytes_));
            if (target_.rasterColors) {
                refusal += QStringLiteral(
                    " Revert to Source Colors first to release the "
                    "current color table, then try again.");
            }
            estimate_->setText(refusal);
            admitted = false;
        }
    } catch (const std::exception &error) {
        estimate_->setText(QString::fromUtf8(error.what()));
        admitted = false;
    }
    setNoticeStyle(*estimate_,
                   admitted ? QStringLiteral("info") : QStringLiteral("error"));

    const Bounds3d &pointBounds = target_.availableBounds;
    const bool disjoint = pointBounds.valid() && metadata.bounds.valid() &&
                          xyDisjoint(pointBounds, metadata.bounds);
    QString warning;
    if (comparator_) {
        const SpatialReferenceRelation relation = comparator_->compare(
            target_.descriptor.metadata.spatialReferenceWkt,
            metadata.spatialReferenceWkt);
        if (relation == SpatialReferenceRelation::Different) {
            warning =
                QStringLiteral(
                    "Coordinate systems differ. Point cloud: %1. Raster: %2. "
                    "Coordinates are used as-is and the result may be wrong.")
                    .arg(shortCrs(
                        target_.descriptor.metadata.spatialReferenceWkt))
                    .arg(shortCrs(metadata.spatialReferenceWkt));
        } else if (relation == SpatialReferenceRelation::Unknown) {
            warning = QStringLiteral("One of the layers reports no usable "
                                     "coordinate system. Overlap "
                                     "cannot be verified.");
        }
    } else {
        warning =
            QStringLiteral("CRS comparison is unavailable in this build.");
    }
    if (disjoint) {
        warning = QStringLiteral("The selected layers have disjoint XY "
                                 "extents; no point can change.");
    }
    warning_->setText(warning);
    warning_->setVisible(!warning.isEmpty());
    setNoticeStyle(*warning_,
                   disjoint ? QStringLiteral("error")
                            : QStringLiteral("warning"));
    apply_->setEnabled(admitted && !disjoint);
}

} // namespace pci
