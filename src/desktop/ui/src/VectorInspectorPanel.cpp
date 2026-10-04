#include "VectorInspectorPanel.h"
#include "InspectorFormatting.h"
namespace pci {
using namespace inspector;
VectorInspectorPanel::VectorInspectorPanel(QWidget *parent)
    : QWidget(parent)
{
    this->setObjectName(QStringLiteral("vectorLayerProperties"));
    auto *vectorLayout = new QVBoxLayout(this);
    vectorLayout->setContentsMargins(0, 0, 0, 0);
    vectorLayout->setSpacing(12);

    vectorVisibilityWarning_ = new QWidget(this);
    vectorVisibilityWarning_->setObjectName(
        QStringLiteral("vectorExtentWarning"));
    vectorVisibilityWarning_->setProperty("notice", "warning");
    auto *warningLayout = new QHBoxLayout(vectorVisibilityWarning_);
    warningLayout->setContentsMargins(10, 8, 8, 8);
    warningLayout->setSpacing(8);
    vectorVisibilityWarningLabel_ = new QLabel(vectorVisibilityWarning_);
    vectorVisibilityWarningLabel_->setObjectName(
        QStringLiteral("vectorExtentWarningLabel"));
    vectorVisibilityWarningLabel_->setWordWrap(true);
    warningLayout->addWidget(vectorVisibilityWarningLabel_, 1);
    vectorShowAnywayButton_ = new QPushButton(QStringLiteral("Show anyway"),
                                              vectorVisibilityWarning_);
    vectorShowAnywayButton_->setObjectName(
        QStringLiteral("vectorShowAnywayButton"));
    vectorShowAnywayButton_->setToolTip(
        QStringLiteral("Show this layer and include its extent in Fit Scene"));
    warningLayout->addWidget(vectorShowAnywayButton_);
    vectorLayout->addWidget(vectorVisibilityWarning_);

    auto *vectorAppearance = new QGroupBox(QStringLiteral("Appearance"), this);
    vectorAppearance->setObjectName(QStringLiteral("vectorAppearanceSection"));
    auto *vectorForm = new QFormLayout(vectorAppearance);
    vectorForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    vectorForm->setVerticalSpacing(8);
    vectorFillColor_ = new VectorColorButton(vectorAppearance);
    vectorFillColor_->setObjectName(QStringLiteral("vectorFillColorButton"));
    vectorFillColor_->setAccessibleName(QStringLiteral("Fill color"));
    vectorStrokeColor_ = new VectorColorButton(vectorAppearance);
    vectorStrokeColor_->setObjectName(
        QStringLiteral("vectorStrokeColorButton"));
    vectorStrokeColor_->setAccessibleName(QStringLiteral("Stroke color"));
    vectorMarkerColor_ = new VectorColorButton(vectorAppearance);
    vectorMarkerColor_->setObjectName(
        QStringLiteral("vectorMarkerColorButton"));
    vectorMarkerColor_->setAccessibleName(QStringLiteral("Marker color"));
    vectorStrokeWidth_ = new QDoubleSpinBox(vectorAppearance);
    vectorStrokeWidth_->setObjectName(
        QStringLiteral("vectorStrokeWidthSpinBox"));
    vectorStrokeWidth_->setRange(0.0, maximumVectorStrokeWidthPixels);
    vectorStrokeWidth_->setDecimals(1);
    vectorStrokeWidth_->setSingleStep(0.5);
    vectorStrokeWidth_->setSuffix(QStringLiteral(" px"));
    vectorStrokeWidth_->setToolTip(
        QStringLiteral("Width in render-target pixels"));
    vectorMarkerSize_ = new QDoubleSpinBox(vectorAppearance);
    vectorMarkerSize_->setObjectName(QStringLiteral("vectorMarkerSizeSpinBox"));
    vectorMarkerSize_->setRange(1.0, maximumVectorMarkerSizePixels);
    vectorMarkerSize_->setDecimals(1);
    vectorMarkerSize_->setSingleStep(1.0);
    vectorMarkerSize_->setSuffix(QStringLiteral(" px"));
    vectorMarkerSize_->setToolTip(
        QStringLiteral("Size in render-target pixels"));
    vectorMarkerShape_ = new QComboBox(vectorAppearance);
    vectorMarkerShape_->setObjectName(
        QStringLiteral("vectorMarkerShapeComboBox"));
    vectorMarkerShape_->addItem(QStringLiteral("Circle"),
                                static_cast<int>(VectorMarkerShape::Circle));
    vectorMarkerShape_->addItem(QStringLiteral("Square"),
                                static_cast<int>(VectorMarkerShape::Square));
    vectorOpacity_ = new QDoubleSpinBox(vectorAppearance);
    vectorOpacity_->setObjectName(QStringLiteral("vectorOpacitySpinBox"));
    vectorOpacity_->setAccessibleName(QStringLiteral("Layer opacity"));
    vectorOpacity_->setRange(0.0, 100.0);
    vectorOpacity_->setDecimals(0);
    vectorOpacity_->setSingleStep(5.0);
    vectorOpacity_->setSuffix(QStringLiteral(" %"));
    vectorOpacity_->setToolTip(
        QStringLiteral("Opacity applied to the complete vector layer"));
    vectorResetStyle_ =
        new QPushButton(QStringLiteral("Reset appearance"), vectorAppearance);
    vectorResetStyle_->setObjectName(QStringLiteral("vectorResetStyleButton"));
    vectorResetStyle_->setToolTip(QStringLiteral(
        "Restore colors, sizes, shape, and opacity for this geometry type"));
    vectorForm->addRow(QStringLiteral("Fill"), vectorFillColor_);
    vectorForm->addRow(QStringLiteral("Stroke"), vectorStrokeColor_);
    vectorForm->addRow(QStringLiteral("Marker"), vectorMarkerColor_);
    vectorForm->addRow(QStringLiteral("Stroke width"), vectorStrokeWidth_);
    vectorForm->addRow(QStringLiteral("Marker size"), vectorMarkerSize_);
    vectorForm->addRow(QStringLiteral("Marker shape"), vectorMarkerShape_);
    vectorForm->addRow(QStringLiteral("Opacity"), vectorOpacity_);
    vectorForm->addRow(vectorResetStyle_);
    vectorLayout->addWidget(vectorAppearance);

    auto *placement = new QGroupBox(QStringLiteral("Placement"), this);
    placement->setObjectName(QStringLiteral("vectorPlacementSection"));
    auto *placementForm = new QFormLayout(placement);
    placementForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    placementForm->setVerticalSpacing(8);
    vectorZOffset_ = new QDoubleSpinBox(placement);
    vectorZOffset_->setObjectName(QStringLiteral("vectorZOffsetSpinBox"));
    vectorZOffset_->setRange(-maximumVectorZOffsetMagnitude,
                             maximumVectorZOffsetMagnitude);
    vectorZOffset_->setDecimals(3);
    vectorZOffset_->setKeyboardTracking(false);
    vectorZOffset_->setToolTip(
        QStringLiteral("Move this planar overlay along the scene Z axis"));
    vectorPlaceAboveScene_ =
        new QPushButton(QStringLiteral("Place above scene"), placement);
    vectorPlaceAboveScene_->setObjectName(
        QStringLiteral("vectorPlaceAboveSceneButton"));
    vectorPlaceAboveScene_->setProperty("primary", true);
    vectorPlaceAboveScene_->setToolTip(QStringLiteral(
        "Place the overlay just above the point-cloud height range"));
    vectorMatchSceneFloor_ =
        new QPushButton(QStringLiteral("Match scene floor"), placement);
    vectorMatchSceneFloor_->setObjectName(
        QStringLiteral("vectorMatchFloorButton"));
    vectorMatchSceneFloor_->setToolTip(QStringLiteral(
        "Place the overlay at the lowest point-cloud elevation"));
    vectorAlwaysOnTop_ =
        new QCheckBox(QStringLiteral("Always on top"), placement);
    vectorAlwaysOnTop_->setObjectName(
        QStringLiteral("vectorAlwaysOnTopCheckBox"));
    vectorAlwaysOnTop_->setToolTip(QStringLiteral(
        "Draw after scene geometry using deterministic painter order"));
    vectorPlacementNotice_ =
        new QLabel(QStringLiteral("Uses one constant elevation; the overlay "
                                  "does not follow terrain."),
                   placement);
    vectorPlacementNotice_->setObjectName(
        QStringLiteral("vectorPlacementNotice"));
    vectorPlacementNotice_->setWordWrap(true);
    placementForm->addRow(QStringLiteral("Elevation offset"), vectorZOffset_);
    placementForm->addRow(vectorPlaceAboveScene_);
    placementForm->addRow(vectorMatchSceneFloor_);
    placementForm->addRow(vectorAlwaysOnTop_);
    placementForm->addRow(vectorPlacementNotice_);
    vectorLayout->addWidget(placement);

    auto *vectorInformation =
        new QGroupBox(QStringLiteral("Information"), this);
    vectorInformation->setObjectName(
        QStringLiteral("vectorInformationSection"));
    auto *vectorInformationForm = new QFormLayout(vectorInformation);
    vectorInformationForm->setFieldGrowthPolicy(
        QFormLayout::ExpandingFieldsGrow);
    vectorFeaturesValue_ = makeValueLabel();
    vectorFeaturesValue_->setObjectName(QStringLiteral("vectorFeaturesValue"));
    vectorGeometryValue_ = makeValueLabel();
    vectorGeometryValue_->setObjectName(QStringLiteral("vectorGeometryValue"));
    vectorDriverValue_ = makeValueLabel();
    vectorDriverValue_->setObjectName(QStringLiteral("vectorDriverValue"));
    vectorCrsValue_ = makeValueLabel();
    vectorCrsValue_->setObjectName(QStringLiteral("vectorCrsValue"));
    vectorBoundsValue_ = makeValueLabel();
    vectorBoundsValue_->setObjectName(QStringLiteral("vectorBoundsValue"));
    vectorSourceValue_ = makeValueLabel();
    vectorSourceValue_->setObjectName(QStringLiteral("vectorSourceValue"));
    vectorInformationForm->addRow(QStringLiteral("Features"),
                                  vectorFeaturesValue_);
    vectorInformationForm->addRow(QStringLiteral("Geometry"),
                                  vectorGeometryValue_);
    vectorInformationForm->addRow(QStringLiteral("Driver"), vectorDriverValue_);
    vectorInformationForm->addRow(QStringLiteral("Reported CRS"),
                                  vectorCrsValue_);
    vectorInformationForm->addRow(QStringLiteral("Size (X×Y×Z)"),
                                  vectorBoundsValue_);
    vectorInformationForm->addRow(QStringLiteral("Source"), vectorSourceValue_);
    vectorLayout->addWidget(vectorInformation);

    for (VectorColorButton *button :
         {vectorFillColor_, vectorStrokeColor_, vectorMarkerColor_}) {
        connect(
            button, &VectorColorButton::colorChanged, this, [this](VectorRgba) {
                applyVectorStyle();
            });
    }
    for (QDoubleSpinBox *spin : {vectorStrokeWidth_,
                                 vectorMarkerSize_,
                                 vectorOpacity_,
                                 vectorZOffset_}) {
        connect(spin,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this,
                [this](double) {
                    applyVectorStyle();
                });
    }
    connect(vectorMarkerShape_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                applyVectorStyle();
            });
    connect(vectorAlwaysOnTop_, &QCheckBox::toggled, this, [this](bool) {
        applyVectorStyle();
    });
    connect(vectorResetStyle_, &QPushButton::clicked, this, [this] {
        const auto id = selectedLayerId();
        const VectorLayerSnapshot *vector = id ? vectorLayerById(*id) : nullptr;
        if (!vector) {
            return;
        }
        const VectorGeometryKind kind =
            vector->data ? vector->data->summary.dominantKind()
                         : VectorGeometryKind::Line;
        const VectorLayerStyle defaults = defaultVectorLayerStyle(kind);
        const QSignalBlocker fillBlock(vectorFillColor_);
        const QSignalBlocker strokeBlock(vectorStrokeColor_);
        const QSignalBlocker markerBlock(vectorMarkerColor_);
        const QSignalBlocker widthBlock(vectorStrokeWidth_);
        const QSignalBlocker sizeBlock(vectorMarkerSize_);
        const QSignalBlocker shapeBlock(vectorMarkerShape_);
        const QSignalBlocker opacityBlock(vectorOpacity_);
        vectorFillColor_->setColor(defaults.fill);
        vectorStrokeColor_->setColor(defaults.stroke);
        vectorMarkerColor_->setColor(defaults.marker);
        vectorStrokeWidth_->setValue(defaults.strokeWidthPixels);
        vectorMarkerSize_->setValue(defaults.markerSizePixels);
        vectorMarkerShape_->setCurrentIndex(vectorMarkerShape_->findData(
            static_cast<int>(defaults.markerShape)));
        vectorOpacity_->setValue(defaults.opacity * 100.0F);
        applyVectorStyle();
    });
    connect(vectorShowAnywayButton_, &QPushButton::clicked, this, [this] {
        const auto id = selectedLayerId();
        if (id && vectorLayerById(*id)) {
            emit showVectorAnywayRequested(*id);
        }
    });
    connect(vectorPlaceAboveScene_, &QPushButton::clicked, this, [this] {
        const Bounds3d sceneBounds = context_.colorBounds;
        if (!sceneBounds.valid()) {
            return;
        }
        const double height = sceneBounds.maximum[2] - sceneBounds.minimum[2];
        vectorZOffset_->setValue(sceneBounds.maximum[2] +
                                 std::max(height * 0.01, 0.01));
    });
    connect(vectorMatchSceneFloor_, &QPushButton::clicked, this, [this] {
        const Bounds3d sceneBounds = context_.colorBounds;
        if (sceneBounds.valid()) {
            vectorZOffset_->setValue(sceneBounds.minimum[2]);
        }
    });
}

void VectorInspectorPanel::present(std::optional<VectorLayerSnapshot> selected,
                                   InspectorContext context)
{
    selected_ = std::move(selected);
    context_ = context;
    setVisible(selected_.has_value());
    updateProperties();
}

void VectorInspectorPanel::updateProperties()
{
    const auto *vector = selected_ ? &*selected_ : nullptr;
    if (!vector)
        return;
    const QSignalBlocker fillBlock(vectorFillColor_);
    const QSignalBlocker strokeBlock(vectorStrokeColor_);
    const QSignalBlocker markerBlock(vectorMarkerColor_);
    const QSignalBlocker widthBlock(vectorStrokeWidth_);
    const QSignalBlocker sizeBlock(vectorMarkerSize_);
    const QSignalBlocker shapeBlock(vectorMarkerShape_);
    const QSignalBlocker opacityBlock(vectorOpacity_);
    const QSignalBlocker offsetBlock(vectorZOffset_);
    const QSignalBlocker topBlock(vectorAlwaysOnTop_);
    vectorFillColor_->setColor(vector->style.fill);
    vectorStrokeColor_->setColor(vector->style.stroke);
    vectorMarkerColor_->setColor(vector->style.marker);
    vectorStrokeWidth_->setValue(vector->style.strokeWidthPixels);
    vectorMarkerSize_->setValue(vector->style.markerSizePixels);
    vectorMarkerShape_->setCurrentIndex(vectorMarkerShape_->findData(
        static_cast<int>(vector->style.markerShape)));
    vectorOpacity_->setValue(vector->style.opacity * 100.0F);
    vectorZOffset_->setValue(vector->style.zOffset);
    vectorAlwaysOnTop_->setChecked(vector->style.alwaysOnTop);
    const bool hiddenDisjoint =
        vector->data && vector->data->extentDisjointXY && !vector->visible;
    const bool crsMismatch = vector->data && vector->data->crsMismatch;
    QStringList warnings;
    if (hiddenDisjoint) {
        warnings << QStringLiteral(
            "This layer does not overlap the scene in X/Y, so it "
            "was added hidden. Fit Scene stays unchanged until you "
            "show it.");
    }
    if (crsMismatch) {
        warnings << QStringLiteral(
            "The CRS reported by GDAL differs from the scene CRS. "
            "Coordinates were not reprojected.");
    }
    vectorVisibilityWarning_->setVisible(!warnings.isEmpty());
    vectorVisibilityWarningLabel_->setText(warnings.join(QStringLiteral("\n")));
    vectorShowAnywayButton_->setVisible(hiddenDisjoint);

    const Bounds3d sceneBounds = context_.colorBounds;
    vectorPlaceAboveScene_->setEnabled(sceneBounds.valid());
    vectorMatchSceneFloor_->setEnabled(sceneBounds.valid());
    QString placementText =
        QStringLiteral("Uses one constant elevation; the overlay does not "
                       "follow terrain.");
    if (sceneBounds.valid() && vector->data && vector->data->bounds.valid()) {
        const double placedMinimum =
            vector->data->bounds.minimum[2] + vector->style.zOffset;
        const double placedMaximum =
            vector->data->bounds.maximum[2] + vector->style.zOffset;
        if (placedMaximum < sceneBounds.minimum[2]) {
            placementText =
                QStringLiteral(
                    "The overlay is %1 below the point-cloud height "
                    "range (%2 to %3). Place it above the scene or use "
                    "Always on top.")
                    .arg(sceneBounds.minimum[2] - placedMaximum, 0, 'f', 2)
                    .arg(sceneBounds.minimum[2], 0, 'f', 2)
                    .arg(sceneBounds.maximum[2], 0, 'f', 2);
        } else if (placedMinimum > sceneBounds.maximum[2]) {
            placementText =
                QStringLiteral("The overlay is %1 above the point-cloud height "
                               "range (%2 to %3).")
                    .arg(placedMinimum - sceneBounds.maximum[2], 0, 'f', 2)
                    .arg(sceneBounds.minimum[2], 0, 'f', 2)
                    .arg(sceneBounds.maximum[2], 0, 'f', 2);
        } else {
            placementText =
                QStringLiteral("Planar elevation intersects the point-cloud "
                               "height range (%1 to %2); it does not follow "
                               "terrain.")
                    .arg(sceneBounds.minimum[2], 0, 'f', 2)
                    .arg(sceneBounds.maximum[2], 0, 'f', 2);
        }
    }
    vectorPlacementNotice_->setText(placementText);
    if (vector->data) {
        const QLocale locale;
        setValueText(vectorFeaturesValue_,
                     locale.toString(
                         static_cast<qulonglong>(vector->data->featureCount)));
        setValueText(
            vectorGeometryValue_,
            QString::fromStdString(vector->data->geometryDescription()));
        setValueText(vectorDriverValue_,
                     vector->data->sourceDriver.empty()
                         ? QStringLiteral("—")
                         : QString::fromStdString(vector->data->sourceDriver));
        setValueText(
            vectorCrsValue_,
            vector->data->spatialReferenceWkt.empty()
                ? QStringLiteral("Not reported")
                : QString::fromStdString(vector->data->spatialReferenceWkt));
        setValueText(vectorBoundsValue_, boundsText(vector->data->bounds));
        setValueText(vectorSourceValue_,
                     vector->data->sourcePath.empty()
                         ? QStringLiteral("—")
                         : pathToQString(vector->data->sourcePath));
    } else {
        for (QLabel *value : {vectorFeaturesValue_,
                              vectorGeometryValue_,
                              vectorDriverValue_,
                              vectorCrsValue_,
                              vectorBoundsValue_,
                              vectorSourceValue_}) {
            setValueText(value, QStringLiteral("—"));
        }
    }
}
void VectorInspectorPanel::applyVectorStyle()
{
    const auto layerId = selectedLayerId();
    const VectorLayerSnapshot *layer =
        layerId ? vectorLayerById(*layerId) : nullptr;
    if (!layer)
        return;
    VectorLayerStyle style = layer->style;
    style.fill = vectorFillColor_->color();
    style.stroke = vectorStrokeColor_->color();
    style.marker = vectorMarkerColor_->color();
    style.strokeWidthPixels = static_cast<float>(vectorStrokeWidth_->value());
    style.markerSizePixels = static_cast<float>(vectorMarkerSize_->value());
    style.markerShape = static_cast<VectorMarkerShape>(
        vectorMarkerShape_->currentData().toInt());
    style.opacity = static_cast<float>(vectorOpacity_->value() / 100.0);
    style.zOffset = vectorZOffset_->value();
    style.alwaysOnTop = vectorAlwaysOnTop_->isChecked();
    if (style != layer->style)
        emit vectorStyleChanged(layer->id, style);
}

} // namespace pci
