#include "RasterInspectorPanel.h"
#include "InspectorFormatting.h"
namespace pci {
using namespace inspector;
RasterInspectorPanel::RasterInspectorPanel(
    QWidget *parent, PointColorMapCatalogSnapshotPtr colorMaps)
    : QWidget(parent)
    , colorMaps_(std::move(colorMaps))
{
    this->setObjectName(QStringLiteral("rasterLayerProperties"));
    auto *rasterLayout = new QVBoxLayout(this);
    rasterLayout->setContentsMargins(0, 0, 0, 0);
    rasterLayout->setSpacing(12);

    rasterVisibilityWarning_ = new QWidget(this);
    rasterVisibilityWarning_->setObjectName(
        QStringLiteral("rasterVisibilityWarning"));
    auto *rasterWarningLayout = new QVBoxLayout(rasterVisibilityWarning_);
    rasterWarningLayout->setContentsMargins(0, 0, 0, 0);
    rasterVisibilityWarningLabel_ = new QLabel(rasterVisibilityWarning_);
    rasterVisibilityWarningLabel_->setObjectName(
        QStringLiteral("rasterVisibilityWarningLabel"));
    rasterVisibilityWarningLabel_->setWordWrap(true);
    rasterShowAnywayButton_ = new QPushButton(QStringLiteral("Show anyway"),
                                              rasterVisibilityWarning_);
    rasterShowAnywayButton_->setObjectName(
        QStringLiteral("rasterShowAnywayButton"));
    rasterWarningLayout->addWidget(rasterVisibilityWarningLabel_);
    rasterWarningLayout->addWidget(rasterShowAnywayButton_);
    rasterVisibilityWarning_->setVisible(false);
    rasterLayout->addWidget(rasterVisibilityWarning_);

    auto *rasterAppearance = new QGroupBox(QStringLiteral("Appearance"), this);
    rasterAppearance->setObjectName(QStringLiteral("rasterAppearanceSection"));
    auto *rasterAppearanceForm = new QFormLayout(rasterAppearance);
    rasterAppearanceForm->setFieldGrowthPolicy(
        QFormLayout::ExpandingFieldsGrow);
    rasterOpacity_ = new QDoubleSpinBox(rasterAppearance);
    rasterOpacity_->setObjectName(QStringLiteral("rasterOpacitySpinBox"));
    rasterOpacity_->setRange(0.0, 100.0);
    rasterOpacity_->setDecimals(0);
    rasterOpacity_->setSuffix(QStringLiteral(" %"));
    rasterOpacity_->setKeyboardTracking(false);
    rasterAppearanceForm->addRow(QStringLiteral("Opacity"), rasterOpacity_);

    // Display range and ramp apply to single-band continuous sources only.
    // Changing either alters decoded pixels, unlike opacity and elevation.
    rasterRangeWidget_ = new QWidget(rasterAppearance);
    rasterRangeWidget_->setObjectName(QStringLiteral("rasterRangeWidget"));
    auto *rasterRangeLayout = new QHBoxLayout(rasterRangeWidget_);
    rasterRangeLayout->setContentsMargins(0, 0, 0, 0);
    rasterRangeMinimum_ = new QDoubleSpinBox(rasterRangeWidget_);
    rasterRangeMinimum_->setObjectName(
        QStringLiteral("rasterRangeMinimumSpinBox"));
    rasterRangeMinimum_->setRange(-1.0e12, 1.0e12);
    rasterRangeMinimum_->setDecimals(3);
    rasterRangeMinimum_->setKeyboardTracking(false);
    rasterRangeMaximum_ = new QDoubleSpinBox(rasterRangeWidget_);
    rasterRangeMaximum_->setObjectName(
        QStringLiteral("rasterRangeMaximumSpinBox"));
    rasterRangeMaximum_->setRange(-1.0e12, 1.0e12);
    rasterRangeMaximum_->setDecimals(3);
    rasterRangeMaximum_->setKeyboardTracking(false);
    rasterRangeLayout->addWidget(rasterRangeMinimum_);
    rasterRangeLayout->addWidget(rasterRangeMaximum_);
    rasterAppearanceForm->addRow(QStringLiteral("Display range"),
                                 rasterRangeWidget_);

    rasterColorRamp_ = new QComboBox(rasterAppearance);
    rasterColorRamp_->setObjectName(QStringLiteral("rasterColorRampCombo"));
    if (colorMaps_) {
        for (const PointColorMapDefinition &definition :
             colorMaps_->definitions()) {
            if (definition.kind != PointColorMapKind::Continuous ||
                definition.stops.empty()) {
                continue;
            }
            rasterColorRamp_->addItem(
                QString::fromUtf8(definition.name.data(),
                                  static_cast<int>(definition.name.size())),
                QString::fromUtf8(definition.key.data(),
                                  static_cast<int>(definition.key.size())));
        }
    }
    rasterAppearanceForm->addRow(QStringLiteral("Color ramp"),
                                 rasterColorRamp_);
    rasterLayout->addWidget(rasterAppearance);

    rasterRenderingWidget_ =
        new QGroupBox(QStringLiteral("Terrain rendering"), this);
    rasterRenderingWidget_->setObjectName(
        QStringLiteral("rasterRenderingSection"));
    auto *rasterRenderingForm = new QFormLayout(rasterRenderingWidget_);
    rasterRenderingForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    rasterRenderMode_ = new QComboBox(rasterRenderingWidget_);
    rasterRenderMode_->setObjectName(QStringLiteral("rasterRenderModeCombo"));
    rasterRenderMode_->addItem(QStringLiteral("Flat"),
                               static_cast<int>(RasterRenderMode::Flat));
    rasterRenderMode_->addItem(QStringLiteral("Surface (true elevation)"),
                               static_cast<int>(RasterRenderMode::Surface));
    rasterVerticalExaggeration_ = new QDoubleSpinBox(rasterRenderingWidget_);
    rasterVerticalExaggeration_->setObjectName(
        QStringLiteral("rasterVerticalExaggerationSpinBox"));
    rasterVerticalExaggeration_->setRange(minimumRasterVerticalExaggeration,
                                          maximumRasterVerticalExaggeration);
    rasterVerticalExaggeration_->setDecimals(1);
    rasterVerticalExaggeration_->setSingleStep(0.5);
    rasterVerticalExaggeration_->setSuffix(QStringLiteral("×"));
    rasterVerticalExaggeration_->setKeyboardTracking(false);
    rasterSurfaceShading_ = new QDoubleSpinBox(rasterRenderingWidget_);
    rasterSurfaceShading_->setObjectName(
        QStringLiteral("rasterSurfaceShadingSpinBox"));
    rasterSurfaceShading_->setRange(0.0, 100.0);
    rasterSurfaceShading_->setDecimals(0);
    rasterSurfaceShading_->setSuffix(QStringLiteral(" %"));
    rasterSurfaceShading_->setKeyboardTracking(false);
    rasterElevationStatus_ = new QLabel(rasterRenderingWidget_);
    rasterElevationStatus_->setObjectName(
        QStringLiteral("rasterElevationStatusLabel"));
    rasterElevationStatus_->setWordWrap(true);
    auto *elevationActions = new QWidget(rasterRenderingWidget_);
    auto *elevationActionsLayout = new QHBoxLayout(elevationActions);
    elevationActionsLayout->setContentsMargins(0, 0, 0, 0);
    rasterElevationRetry_ =
        new QPushButton(QStringLiteral("Retry analysis"), elevationActions);
    rasterElevationRetry_->setObjectName(
        QStringLiteral("rasterElevationRetryButton"));
    rasterElevationCancel_ =
        new QPushButton(QStringLiteral("Cancel analysis"), elevationActions);
    rasterElevationCancel_->setObjectName(
        QStringLiteral("rasterElevationCancelButton"));
    elevationActionsLayout->addWidget(rasterElevationRetry_);
    elevationActionsLayout->addWidget(rasterElevationCancel_);
    rasterRenderingForm->addRow(QStringLiteral("Mode"), rasterRenderMode_);
    rasterRenderingForm->addRow(QStringLiteral("Vertical exaggeration"),
                                rasterVerticalExaggeration_);
    rasterRenderingForm->addRow(QStringLiteral("Surface shading"),
                                rasterSurfaceShading_);
    rasterRenderingForm->addRow(QStringLiteral("Elevation"),
                                rasterElevationStatus_);
    rasterRenderingForm->addRow(elevationActions);
    rasterLayout->addWidget(rasterRenderingWidget_);

    auto *rasterPlacement = new QGroupBox(QStringLiteral("Placement"), this);
    rasterPlacement->setObjectName(QStringLiteral("rasterPlacementSection"));
    auto *rasterPlacementForm = new QFormLayout(rasterPlacement);
    rasterPlacementForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    rasterZOffset_ = new QDoubleSpinBox(rasterPlacement);
    rasterZOffset_->setObjectName(QStringLiteral("rasterZOffsetSpinBox"));
    rasterZOffset_->setRange(-maximumRasterZOffsetMagnitude,
                             maximumRasterZOffsetMagnitude);
    rasterZOffset_->setDecimals(3);
    rasterZOffset_->setKeyboardTracking(false);
    rasterPlaceAboveScene_ =
        new QPushButton(QStringLiteral("Place above scene"), rasterPlacement);
    rasterPlaceAboveScene_->setObjectName(
        QStringLiteral("rasterPlaceAboveSceneButton"));
    rasterPlaceAboveScene_->setProperty("primary", true);
    rasterMatchSceneFloor_ =
        new QPushButton(QStringLiteral("Match scene floor"), rasterPlacement);
    rasterMatchSceneFloor_->setObjectName(
        QStringLiteral("rasterMatchFloorButton"));
    rasterResetElevation_ =
        new QPushButton(QStringLiteral("Reset to z = 0"), rasterPlacement);
    rasterResetElevation_->setObjectName(
        QStringLiteral("rasterResetElevationButton"));
    rasterPlacementForm->addRow(QStringLiteral("Elevation offset"),
                                rasterZOffset_);
    rasterPlacementForm->addRow(rasterPlaceAboveScene_);
    rasterPlacementForm->addRow(rasterMatchSceneFloor_);
    rasterPlacementForm->addRow(rasterResetElevation_);
    rasterLayout->addWidget(rasterPlacement);

    auto *rasterInformation =
        new QGroupBox(QStringLiteral("Information"), this);
    rasterInformation->setObjectName(
        QStringLiteral("rasterInformationSection"));
    auto *rasterInformationForm = new QFormLayout(rasterInformation);
    rasterInformationForm->setFieldGrowthPolicy(
        QFormLayout::ExpandingFieldsGrow);
    rasterDimensionsValue_ = makeValueLabel();
    rasterDimensionsValue_->setObjectName(
        QStringLiteral("rasterDimensionsValue"));
    rasterBandsValue_ = makeValueLabel();
    rasterBandsValue_->setObjectName(QStringLiteral("rasterBandsValue"));
    rasterPixelSizeValue_ = makeValueLabel();
    rasterPixelSizeValue_->setObjectName(
        QStringLiteral("rasterPixelSizeValue"));
    rasterOverviewsValue_ = makeValueLabel();
    rasterOverviewsValue_->setObjectName(
        QStringLiteral("rasterOverviewsValue"));
    rasterRangeValue_ = makeValueLabel();
    rasterRangeValue_->setObjectName(QStringLiteral("rasterRangeValue"));
    rasterDriverValue_ = makeValueLabel();
    rasterDriverValue_->setObjectName(QStringLiteral("rasterDriverValue"));
    rasterCrsValue_ = makeValueLabel();
    rasterCrsValue_->setObjectName(QStringLiteral("rasterCrsValue"));
    rasterBoundsValue_ = makeValueLabel();
    rasterBoundsValue_->setObjectName(QStringLiteral("rasterBoundsValue"));
    rasterSourceValue_ = makeValueLabel();
    rasterSourceValue_->setObjectName(QStringLiteral("rasterSourceValue"));
    rasterInformationForm->addRow(QStringLiteral("Dimensions"),
                                  rasterDimensionsValue_);
    rasterInformationForm->addRow(QStringLiteral("Bands"), rasterBandsValue_);
    rasterInformationForm->addRow(QStringLiteral("Native pixel size"),
                                  rasterPixelSizeValue_);
    rasterInformationForm->addRow(QStringLiteral("Overviews"),
                                  rasterOverviewsValue_);
    rasterInformationForm->addRow(QStringLiteral("Applied range"),
                                  rasterRangeValue_);
    rasterInformationForm->addRow(QStringLiteral("Driver"), rasterDriverValue_);
    rasterInformationForm->addRow(QStringLiteral("Reported CRS"),
                                  rasterCrsValue_);
    rasterInformationForm->addRow(QStringLiteral("Size (X×Y)"),
                                  rasterBoundsValue_);
    rasterInformationForm->addRow(QStringLiteral("Source"), rasterSourceValue_);
    rasterLayout->addWidget(rasterInformation);

    for (QDoubleSpinBox *spin : {rasterOpacity_,
                                 rasterZOffset_,
                                 rasterRangeMinimum_,
                                 rasterRangeMaximum_,
                                 rasterVerticalExaggeration_,
                                 rasterSurfaceShading_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
            applyRasterStyle();
        });
    }
    connect(rasterColorRamp_, &QComboBox::currentIndexChanged, this, [this] {
        applyRasterStyle();
    });
    connect(rasterRenderMode_, &QComboBox::currentIndexChanged, this, [this] {
        applyRasterStyle();
    });
    connect(rasterElevationRetry_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedLayerId()) {
            emit retryRasterElevationRequested(*id);
        }
    });
    connect(rasterElevationCancel_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedLayerId()) {
            emit cancelRasterElevationRequested(*id);
        }
    });
    // Elevation presets resolve against scene bounds at the moment they are
    // applied; they do not track later scene changes.
    connect(rasterPlaceAboveScene_, &QPushButton::clicked, this, [this] {
        const Bounds3d bounds = context_.colorBounds;
        const auto id = selectedLayerId();
        const RasterLayerSnapshot *raster = id ? rasterLayerById(*id) : nullptr;
        if (bounds.valid() && raster) {
            double sourceMinimum = 0.0;
            if (raster->style.renderMode == RasterRenderMode::Surface &&
                raster->elevationStatus == RasterElevationStatus::Ready &&
                raster->exactElevationRange) {
                sourceMinimum = raster->exactElevationRange->minimum *
                                raster->style.verticalExaggeration;
            }
            rasterZOffset_->setValue(bounds.maximum[2] - sourceMinimum +
                                     0.01 * bounds.maximumExtent());
        }
    });
    connect(rasterMatchSceneFloor_, &QPushButton::clicked, this, [this] {
        const Bounds3d bounds = context_.colorBounds;
        const auto id = selectedLayerId();
        const RasterLayerSnapshot *raster = id ? rasterLayerById(*id) : nullptr;
        if (bounds.valid() && raster) {
            double sourceMinimum = 0.0;
            if (raster->style.renderMode == RasterRenderMode::Surface &&
                raster->elevationStatus == RasterElevationStatus::Ready &&
                raster->exactElevationRange) {
                sourceMinimum = raster->exactElevationRange->minimum *
                                raster->style.verticalExaggeration;
            }
            rasterZOffset_->setValue(bounds.minimum[2] - sourceMinimum);
        }
    });
    connect(rasterResetElevation_, &QPushButton::clicked, this, [this] {
        rasterZOffset_->setValue(0.0);
    });
    connect(rasterShowAnywayButton_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedLayerId()) {
            emit showRasterAnywayRequested(*id);
        }
    });
}

void RasterInspectorPanel::present(std::optional<RasterLayerSnapshot> selected,
                                   InspectorContext context)
{
    selected_ = std::move(selected);
    context_ = context;
    setVisible(selected_.has_value());
    updateProperties();
}

void RasterInspectorPanel::updateProperties()
{
    if (!selected_)
        return;
    const auto &raster = *selected_;
    const RasterLayerMetadata &metadata = raster.descriptor.metadata;
    const bool scalar = metadata.defaultDisplay.sampleKind ==
                        RasterSampleKind::ContinuousScalar;

    const QSignalBlocker opacityBlock(rasterOpacity_);
    const QSignalBlocker offsetBlock(rasterZOffset_);
    const QSignalBlocker minimumBlock(rasterRangeMinimum_);
    const QSignalBlocker maximumBlock(rasterRangeMaximum_);
    const QSignalBlocker rampBlock(rasterColorRamp_);
    const QSignalBlocker modeBlock(rasterRenderMode_);
    const QSignalBlocker exaggerationBlock(rasterVerticalExaggeration_);
    const QSignalBlocker shadingBlock(rasterSurfaceShading_);

    rasterOpacity_->setValue(raster.style.opacity * 100.0F);
    rasterZOffset_->setValue(raster.style.zOffset);
    const bool elevationEligible = metadata.elevation.available;
    rasterRenderingWidget_->setVisible(elevationEligible);
    const int surfaceIndex = rasterRenderMode_->findData(
        static_cast<int>(RasterRenderMode::Surface));
    if (auto *model =
            qobject_cast<QStandardItemModel *>(rasterRenderMode_->model());
        model && surfaceIndex >= 0) {
        QStandardItem *item = model->item(surfaceIndex);
        item->setEnabled(rasterSurfaceCapability_ ==
                         RasterSurfaceCapability::Supported);
        item->setToolTip(rasterSurfaceCapability_ ==
                                 RasterSurfaceCapability::Unsupported
                             ? rasterSurfaceCapabilityReason_
                             : QString{});
    }
    const int modeIndex =
        rasterRenderMode_->findData(static_cast<int>(raster.style.renderMode));
    if (modeIndex >= 0) {
        rasterRenderMode_->setCurrentIndex(modeIndex);
    }
    rasterVerticalExaggeration_->setValue(raster.style.verticalExaggeration);
    rasterSurfaceShading_->setValue(raster.style.surfaceShadingStrength *
                                    100.0F);
    const bool surfaceRequested =
        raster.style.renderMode == RasterRenderMode::Surface;
    rasterVerticalExaggeration_->setVisible(surfaceRequested);
    rasterSurfaceShading_->setVisible(surfaceRequested);

    QString elevationStatus;
    switch (raster.elevationStatus) {
    case RasterElevationStatus::NotApplicable:
        elevationStatus = QStringLiteral("Not an elevation source");
        break;
    case RasterElevationStatus::Unknown:
        elevationStatus = QStringLiteral(
            "Exact range will be analyzed when Surface is selected.");
        break;
    case RasterElevationStatus::Scanning:
        elevationStatus = QStringLiteral(
            "Analyzing the exact elevation range… Flat remains active.");
        break;
    case RasterElevationStatus::Ready:
        if (raster.exactElevationRange) {
            elevationStatus =
                QStringLiteral("%1 to %2%3")
                    .arg(raster.exactElevationRange->minimum, 0, 'g', 8)
                    .arg(raster.exactElevationRange->maximum, 0, 'g', 8)
                    .arg(metadata.elevation.unit.empty()
                             ? QString{}
                             : QStringLiteral(" %1").arg(QString::fromStdString(
                                   metadata.elevation.unit)));
        } else {
            elevationStatus = QStringLiteral("Exact range ready");
        }
        break;
    case RasterElevationStatus::Failed:
        elevationStatus = raster.elevationFailure.empty()
                              ? QStringLiteral("Elevation analysis failed")
                              : QString::fromStdString(raster.elevationFailure);
        break;
    }
    if (surfaceRequested &&
        rasterSurfaceCapability_ == RasterSurfaceCapability::Unsupported) {
        elevationStatus += QStringLiteral("\nSurface unavailable: %1")
                               .arg(rasterSurfaceCapabilityReason_);
    }
    rasterElevationStatus_->setText(elevationStatus);
    rasterElevationRetry_->setVisible(raster.elevationStatus ==
                                      RasterElevationStatus::Failed);
    rasterElevationCancel_->setVisible(raster.elevationStatus ==
                                       RasterElevationStatus::Scanning);

    // Range and ramp exist only for a single continuous scalar; an RGB source
    // has no meaningful single range and a palette is looked up, not stretched.
    rasterRangeWidget_->setVisible(scalar);
    rasterColorRamp_->setVisible(scalar);
    const std::optional<RasterDisplayRange> range =
        raster.style.displayRange ? raster.style.displayRange
                                  : metadata.defaultDisplay.displayRange;
    if (range) {
        rasterRangeMinimum_->setValue(range->minimum);
        rasterRangeMaximum_->setValue(range->maximum);
    }
    if (const int index = rasterColorRamp_->findData(
            QString::fromStdString(raster.style.colorRampKey));
        index >= 0) {
        rasterColorRamp_->setCurrentIndex(index);
    }

    const bool hiddenDisjoint = metadata.extentDisjointXY && !raster.visible;
    QStringList warnings;
    if (hiddenDisjoint) {
        warnings << QStringLiteral(
            "This layer does not overlap the scene in X/Y, so it was added "
            "hidden. Fit Scene stays unchanged until you show it.");
    }
    if (metadata.crsMismatch) {
        warnings << QStringLiteral(
            "The CRS reported by GDAL differs from the scene CRS. "
            "Coordinates were not reprojected.");
    }
    if (metadata.crsMissing) {
        warnings << QStringLiteral(
            "This raster reports no CRS, so it cannot be compared with the "
            "scene.");
    }
    if (metadata.insufficientOverviews) {
        warnings << QStringLiteral(
            "This raster has no source overviews coarse enough for efficient "
            "zoomed-out display. An automatic low-resolution preview is "
            "used; add source overviews for faster, sharper navigation.");
    }
    if (metadata.positionalBandFallback) {
        warnings << QStringLiteral(
            "This raster does not label its color bands. Bands 1–3 were "
            "interpreted as RGB by position; verify the displayed colors.");
    }
    if (surfaceRequested && metadata.elevation.unit.empty()) {
        warnings << QStringLiteral(
            "This DEM reports no vertical unit. Elevation is used without "
            "implicit conversion.");
    }
    if (surfaceRequested && metadata.geographicCrs) {
        warnings << QStringLiteral(
            "This DEM uses geographic X/Y coordinates. Horizontal degrees "
            "and linear elevation units are incompatible; reproject the "
            "source for physically meaningful relief.");
    }
    rasterVisibilityWarning_->setVisible(!warnings.isEmpty());
    rasterVisibilityWarningLabel_->setText(warnings.join(QChar::LineFeed));
    rasterShowAnywayButton_->setVisible(hiddenDisjoint);

    const Bounds3d sceneBounds = context_.colorBounds;
    const bool placementReady =
        !surfaceRequested ||
        (raster.elevationStatus == RasterElevationStatus::Ready &&
         raster.exactElevationRange.has_value());
    rasterPlaceAboveScene_->setEnabled(sceneBounds.valid() && placementReady);
    rasterMatchSceneFloor_->setEnabled(sceneBounds.valid() && placementReady);

    rasterDimensionsValue_->setText(
        QStringLiteral("%1 x %2").arg(metadata.width).arg(metadata.height));
    rasterBandsValue_->setText(QString::number(metadata.bands.size()));
    rasterPixelSizeValue_->setText(
        QStringLiteral("%1 x %2")
            .arg(metadata.nativePixelSize[0] > 0.0
                     ? metadata.nativePixelSize[0]
                     : std::hypot(metadata.geoTransform[1],
                                  metadata.geoTransform[4]),
                 0,
                 'g',
                 4)
            .arg(metadata.nativePixelSize[1] > 0.0
                     ? metadata.nativePixelSize[1]
                     : std::hypot(metadata.geoTransform[2],
                                  metadata.geoTransform[5]),
                 0,
                 'g',
                 4));
    const std::size_t backedLevels = rasterBackedLevelCount(metadata.levels);
    const std::size_t generatedLevels =
        rasterGeneratedLevelCount(metadata.levels);
    const QString automaticCoverage =
        generatedLevels == 0 ? QString{}
                             : QStringLiteral("automatic coverage %1 x %2")
                                   .arg(metadata.levels.back().width)
                                   .arg(metadata.levels.back().height);
    if (backedLevels <= 1) {
        rasterOverviewsValue_->setText(
            automaticCoverage.isEmpty()
                ? QStringLiteral("None")
                : QStringLiteral("None · %1").arg(automaticCoverage));
    } else {
        const RasterLevel &coarsestBacked = metadata.levels[backedLevels - 1];
        QString text = QStringLiteral("%1 · coarsest source %2 x %3")
                           .arg(backedLevels - 1)
                           .arg(coarsestBacked.width)
                           .arg(coarsestBacked.height);
        if (!automaticCoverage.isEmpty()) {
            text += QStringLiteral(" · %1").arg(automaticCoverage);
        }
        rasterOverviewsValue_->setText(text);
    }
    if (range) {
        // The provenance matters: a sampled range is an estimate, not the
        // dataset's declared extremes.
        QString origin = QStringLiteral("metadata");
        if (range->origin == RasterDisplayRange::Origin::CachedStatistics) {
            origin = QStringLiteral("cached statistics");
        } else if (range->origin == RasterDisplayRange::Origin::Sampled) {
            origin = QStringLiteral("bounded sample");
        }
        rasterRangeValue_->setText(QStringLiteral("%1 to %2 (%3)")
                                       .arg(range->minimum, 0, 'g', 6)
                                       .arg(range->maximum, 0, 'g', 6)
                                       .arg(origin));
    } else {
        rasterRangeValue_->setText(QStringLiteral("—"));
    }
    rasterDriverValue_->setText(QString::fromStdString(metadata.sourceDriver));
    rasterCrsValue_->setText(metadata.spatialReferenceWkt.empty()
                                 ? QStringLiteral("Not reported")
                                 : QStringLiteral("Reported"));
    rasterBoundsValue_->setText(
        QStringLiteral("%1 x %2")
            .arg(metadata.bounds.maximum[0] - metadata.bounds.minimum[0],
                 0,
                 'f',
                 2)
            .arg(metadata.bounds.maximum[1] - metadata.bounds.minimum[1],
                 0,
                 'f',
                 2));
    rasterSourceValue_->setText(pathToQString(metadata.sourcePath));
}

void RasterInspectorPanel::applyRasterStyle()
{
    const auto layerId = selectedLayerId();
    const RasterLayerSnapshot *raster =
        layerId ? rasterLayerById(*layerId) : nullptr;
    if (!raster) {
        return;
    }
    RasterLayerStyle style = raster->style;
    style.opacity = static_cast<float>(rasterOpacity_->value() / 100.0);
    style.zOffset = rasterZOffset_->value();
    if (rasterRenderMode_->currentData().isValid()) {
        style.renderMode = static_cast<RasterRenderMode>(
            rasterRenderMode_->currentData().toInt());
    }
    style.verticalExaggeration = rasterVerticalExaggeration_->value();
    style.surfaceShadingStrength =
        static_cast<float>(rasterSurfaceShading_->value() / 100.0);
    if (raster->descriptor.metadata.defaultDisplay.sampleKind ==
        RasterSampleKind::ContinuousScalar) {
        style.displayRange =
            RasterDisplayRange{.minimum = rasterRangeMinimum_->value(),
                               .maximum = rasterRangeMaximum_->value(),
                               .origin = RasterDisplayRange::Origin::Metadata};
        const QVariant rampKey = rasterColorRamp_->currentData();
        if (rampKey.isValid()) {
            style.colorRampKey = rampKey.toString().toStdString();
        }
    }
    emit rasterStyleChanged(*layerId, clampRasterLayerStyle(std::move(style)));
}

} // namespace pci
