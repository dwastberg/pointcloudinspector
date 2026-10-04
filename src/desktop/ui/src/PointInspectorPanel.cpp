#include "PointInspectorPanel.h"
#include "InspectorFormatting.h"
namespace pci {
using namespace inspector;
PointInspectorPanel::PointInspectorPanel(
    QWidget *parent, PointColorMapCatalogSnapshotPtr colorMaps)
    : QWidget(parent)
    , colorMaps_(std::move(colorMaps))
{
    this->setObjectName(QStringLiteral("pointCloudLayerProperties"));
    auto *propertiesLayout = new QVBoxLayout(this);
    propertiesLayout->setContentsMargins(0, 0, 0, 0);
    propertiesLayout->setSpacing(12);

    auto *appearanceGroup = new QGroupBox(QStringLiteral("Appearance"), this);
    appearanceGroup->setObjectName(
        QStringLiteral("pointCloudAppearanceSection"));
    auto *appearanceForm = new QFormLayout(appearanceGroup);
    appearanceForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    pointsValue_ = makeValueLabel();
    pointsValue_->setObjectName(QStringLiteral("layerPointsValue"));
    sourceValue_ = makeValueLabel();
    sourceValue_->setObjectName(QStringLiteral("layerSourceValue"));
    crsValue_ = makeValueLabel();
    crsValue_->setObjectName(QStringLiteral("layerCrsValue"));
    boundsValue_ = makeValueLabel();
    boundsValue_->setObjectName(QStringLiteral("layerBoundsValue"));
    attributesValue_ = makeValueLabel();
    attributesValue_->setObjectName(QStringLiteral("layerAttributesValue"));
    rasterColorStateValue_ = makeValueLabel();
    rasterColorStateValue_->setObjectName(
        QStringLiteral("layerRasterColorStateValue"));
    rasterColorsValue_ = makeValueLabel();
    rasterColorsValue_->setObjectName(QStringLiteral("layerRasterColorsValue"));
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    indexValue_ = makeValueLabel();
    indexValue_->setObjectName(QStringLiteral("layerIndexValue"));
#endif
    // Fix the combo width to a character count so differing item lengths
    // between layers never resize the dock; longer entries elide.
    const auto stabilizeCombo = [](QComboBox *combo) {
        combo->setSizeAdjustPolicy(
            QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(18);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };
    colorSourceCombo_ = new QComboBox(appearanceGroup);
    colorSourceCombo_->setObjectName(QStringLiteral("colorSourceComboBox"));
    colorSourceCombo_->setAccessibleName(QStringLiteral("Color by"));
    stabilizeCombo(colorSourceCombo_);
    colorMapCombo_ = new QComboBox(appearanceGroup);
    colorMapCombo_->setObjectName(QStringLiteral("colorMapComboBox"));
    colorMapCombo_->setAccessibleName(QStringLiteral("Color map"));
    stabilizeCombo(colorMapCombo_);
    applyColorToAllButton_ = new QPushButton(
        QStringLiteral("Apply appearance to all layers"), appearanceGroup);
    applyColorToAllButton_->setObjectName(
        QStringLiteral("applyColorToAllButton"));
    applyColorToAllButton_->setToolTip(QStringLiteral(
        "Use this color source and color map for every compatible layer"));
    colorizeFromRasterButton_ = new QPushButton(appearanceGroup);
    colorizeFromRasterButton_->setObjectName(
        QStringLiteral("inspectorColorizeFromRasterButton"));
    colorizeFromRasterButton_->setProperty("primary", true);
    colorizeFromRasterButton_->setIcon(
        toolbarIcon(ToolbarIcon::ColorizeRaster, palette()));
    revertRasterColorsButton_ = new QPushButton(
        QStringLiteral("Revert to Source Colors"), appearanceGroup);
    revertRasterColorsButton_->setObjectName(
        QStringLiteral("revertRasterColorsButton"));
    revertRasterColorsButton_->setIcon(
        toolbarIcon(ToolbarIcon::RevertColors, palette()));
    revertRasterColorsButton_->setToolTip(
        QStringLiteral("Restore the point cloud's exact source colors"));

    colorRangeRowLabel_ = new QLabel(QStringLiteral("Range"), appearanceGroup);
    colorRangeWidget_ = new QWidget(appearanceGroup);
    colorRangeWidget_->setObjectName(QStringLiteral("colorRangeEditor"));
    auto *rangeLayout = new QGridLayout(colorRangeWidget_);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->setColumnStretch(1, 1);
    automaticColorRangeCheck_ =
        new QCheckBox(QStringLiteral("Automatic range"), colorRangeWidget_);
    automaticColorRangeCheck_->setObjectName(
        QStringLiteral("automaticColorRangeCheckBox"));
    colorRangeMinimumSpin_ = new QDoubleSpinBox(colorRangeWidget_);
    colorRangeMinimumSpin_->setObjectName(
        QStringLiteral("colorRangeMinimumSpinBox"));
    colorRangeMinimumSpin_->setAccessibleName(
        QStringLiteral("Color range minimum"));
    colorRangeMaximumSpin_ = new QDoubleSpinBox(colorRangeWidget_);
    colorRangeMaximumSpin_->setObjectName(
        QStringLiteral("colorRangeMaximumSpinBox"));
    colorRangeMaximumSpin_->setAccessibleName(
        QStringLiteral("Color range maximum"));
    for (QDoubleSpinBox *spin :
         {colorRangeMinimumSpin_, colorRangeMaximumSpin_}) {
        spin->setRange(-std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::max());
        spin->setDecimals(6);
        spin->setKeyboardTracking(false);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    rangeLayout->addWidget(automaticColorRangeCheck_, 0, 0, 1, 2);
    rangeLayout->addWidget(
        new QLabel(QStringLiteral("Min"), colorRangeWidget_), 1, 0);
    rangeLayout->addWidget(colorRangeMinimumSpin_, 1, 1);
    rangeLayout->addWidget(
        new QLabel(QStringLiteral("Max"), colorRangeWidget_), 2, 0);
    rangeLayout->addWidget(colorRangeMaximumSpin_, 2, 1);
    appearanceForm->addRow(QStringLiteral("Color by"), colorSourceCombo_);
    appearanceForm->addRow(QStringLiteral("Color map"), colorMapCombo_);
    appearanceForm->addRow(QStringLiteral("Raster colors"),
                           rasterColorStateValue_);
    appearanceForm->addRow(colorizeFromRasterButton_);
    appearanceForm->addRow(revertRasterColorsButton_);
    appearanceForm->addRow(colorRangeRowLabel_, colorRangeWidget_);
    appearanceForm->addRow(applyColorToAllButton_);
    propertiesLayout->addWidget(appearanceGroup);

    auto *filtersGroup = new QGroupBox(QStringLiteral("Filters"), this);
    filtersGroup->setObjectName(QStringLiteral("pointCloudFiltersSection"));
    auto *filtersLayout = new QVBoxLayout(filtersGroup);
    classificationFilterButton_ = new QPushButton(
        QStringLiteral("Filter classifications…"), filtersGroup);
    classificationFilterButton_->setObjectName(
        QStringLiteral("classificationFilterButton"));
    filtersLayout->addWidget(classificationFilterButton_);
    propertiesLayout->addWidget(filtersGroup);

    auto *informationGroup = new QGroupBox(QStringLiteral("Information"), this);
    informationGroup->setObjectName(
        QStringLiteral("pointCloudInformationSection"));
    auto *informationForm = new QFormLayout(informationGroup);
    informationForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    informationForm->addRow(QStringLiteral("Points"), pointsValue_);
    informationForm->addRow(QStringLiteral("Attributes"), attributesValue_);
    informationForm->addRow(QStringLiteral("Raster colors"),
                            rasterColorsValue_);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    informationForm->addRow(QStringLiteral("Local index"), indexValue_);
#endif
    informationForm->addRow(QStringLiteral("Size (X×Y×Z)"), boundsValue_);
    informationForm->addRow(QStringLiteral("CRS"), crsValue_);
    informationForm->addRow(QStringLiteral("Source"), sourceValue_);
    propertiesLayout->addWidget(informationGroup);
    connect(colorSourceCombo_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                applyColorSource(index);
            });
    connect(colorMapCombo_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                updatePointColorMapToolTip(*colorMapCombo_);
                applyColorMap(index);
            });
    connect(applyColorToAllButton_, &QPushButton::clicked, this, [this] {
        applyColorToAll();
    });
    connect(colorizeFromRasterButton_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedLayerId()) {
            emit colorizeFromRasterRequested(*id);
        }
    });
    connect(revertRasterColorsButton_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedLayerId()) {
            emit revertRasterColorsRequested(*id);
        }
    });
    connect(classificationFilterButton_, &QPushButton::clicked, this, [this] {
        showClassificationFilterDialog();
    });
    connect(automaticColorRangeCheck_,
            &QCheckBox::toggled,
            this,
            [this](const bool automatic) {
                applyAutomaticColorRange(automatic);
            });
    connect(colorRangeMinimumSpin_,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this,
            [this](const double minimum) {
                if (!automaticColorRangeCheck_->isChecked() &&
                    minimum >= colorRangeMaximumSpin_->value()) {
                    const QSignalBlocker blocker(colorRangeMaximumSpin_);
                    colorRangeMaximumSpin_->setValue(
                        minimum +
                        std::max(colorRangeMinimumSpin_->singleStep(), 1e-9));
                }
                applyColorRange();
            });
    connect(colorRangeMaximumSpin_,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this,
            [this](const double maximum) {
                if (!automaticColorRangeCheck_->isChecked() &&
                    maximum <= colorRangeMinimumSpin_->value()) {
                    const QSignalBlocker blocker(colorRangeMinimumSpin_);
                    colorRangeMinimumSpin_->setValue(
                        maximum -
                        std::max(colorRangeMaximumSpin_->singleStep(), 1e-9));
                }
                applyColorRange();
            });
}

void PointInspectorPanel::present(
    std::optional<PointCloudLayerSnapshot> selected, InspectorContext context)
{
    selected_ = std::move(selected);
    context_ = context;
    setVisible(selected_.has_value());
    updateProperties();
}

void PointInspectorPanel::updateProperties()
{
    const auto *layer = selected_ ? &*selected_ : nullptr;
    const RasterColorizeUiState colorizeState =
        rasterColorizeUiState(layer,
                              context_.rasterCount,
                              context_.colorizeActive,
                              context_.colorizeCommitting);
    colorizeFromRasterButton_->setText(colorizeState.startText);
    colorizeFromRasterButton_->setToolTip(colorizeState.startToolTip);
    colorizeFromRasterButton_->setEnabled(colorizeState.startEnabled);
    revertRasterColorsButton_->setVisible(colorizeState.revertVisible);
    revertRasterColorsButton_->setEnabled(colorizeState.revertEnabled);
    revertRasterColorsButton_->setToolTip(colorizeState.revertToolTip);
    if (!layer)
        return;
    const PointCloudMetadata &metadata = layer->descriptor.metadata;
    const QLocale locale;
    const std::uint64_t resident = layer->availablePointCount;
    const std::uint64_t expected =
        std::max<std::uint64_t>(resident, metadata.sourcePointCount);
    setValueText(
        pointsValue_,
        resident == expected
            ? locale.toString(static_cast<qulonglong>(resident))
            : QStringLiteral("%1 / %2")
                  .arg(locale.toString(static_cast<qulonglong>(resident)))
                  .arg(locale.toString(static_cast<qulonglong>(expected))));

    const QString sourcePath = pathToQString(metadata.sourcePath);
    setValueText(sourceValue_,
                 sourcePath.isEmpty() ? QStringLiteral("—") : sourcePath);

    const QString crs = QString::fromStdString(metadata.spatialReferenceWkt);
    setValueText(crsValue_, crs.isEmpty() ? QStringLiteral("—") : crs);

    setValueText(boundsValue_, boundsText(layer->availableBounds));
    setValueText(attributesValue_, attributesText(metadata));
    if (const auto &binding = layer->rasterColors) {
        QString relation = QStringLiteral("CRS comparison unavailable");
        if (binding->crsRelation) {
            switch (*binding->crsRelation) {
            case SpatialReferenceRelation::Same:
                relation = QStringLiteral("same CRS");
                break;
            case SpatialReferenceRelation::Different:
                relation = QStringLiteral("different CRS");
                break;
            case SpatialReferenceRelation::Unknown:
                relation = QStringLiteral("CRS unknown");
                break;
            }
        }
        const QString source =
            binding->rasterSourcePath.empty()
                ? QStringLiteral("Raster source")
                : pathToQString(binding->rasterSourcePath.filename());
        setValueText(rasterColorStateValue_,
                     QStringLiteral("Raster colors · %1").arg(source));
        const QString transform =
            binding->decode ? rasterDecodeText(*binding->decode)
                            : QStringLiteral("display transform unavailable");
        setValueText(
            rasterColorsValue_,
            QStringLiteral("%1 · %2 colored · %3 kept source · %4 · %5")
                .arg(transform)
                .arg(locale.toString(
                    static_cast<qulonglong>(binding->coloredPoints)))
                .arg(locale.toString(
                    static_cast<qulonglong>(binding->uncoloredPoints)))
                .arg(relation)
                .arg(binding->rasterLayerId
                         ? QStringLiteral("linked")
                         : QStringLiteral("source layer removed")));
    } else {
        setValueText(rasterColorStateValue_, QStringLiteral("Source colors"));
        setValueText(rasterColorsValue_, QStringLiteral("—"));
    }
    classificationFilterButton_->setVisible(metadata.hasClassification);
    if (classificationFilterButton_->parentWidget()) {
        classificationFilterButton_->parentWidget()->setVisible(
            metadata.hasClassification);
    }
    if (metadata.hasClassification) {
        const PointClassificationFilter available = context_.classifications;
        const std::size_t availableCount = available.visibleCount();
        const std::size_t visible =
            visibleClassificationCount(layer->classificationFilter, available);
        classificationFilterButton_->setText(
            visible == availableCount
                ? QStringLiteral("Filter classifications…")
                : QStringLiteral("Classifications: %1 of %2 visible…")
                      .arg(visible)
                      .arg(availableCount));
        classificationFilterButton_->setToolTip(
            QStringLiteral("Choose which point classifications to show."));
    }
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    const PointCloudStorageMetrics &storage = layer->storage;
    if (!storage.localPersistent) {
        setValueText(indexValue_, QStringLiteral("Not paged"));
    } else {
        setValueText(
            indexValue_,
            QStringLiteral("%1 MiB · %2%3")
                .arg(static_cast<double>(storage.persistentBytes) /
                         (1024.0 * 1024.0),
                     0,
                     'f',
                     1)
                .arg(storage.committed ? QStringLiteral("committed")
                                       : QStringLiteral("building"))
                .arg(storage.reused ? QStringLiteral(" · reused") : QString{}));
    }
#endif

    const QSignalBlocker sourceBlock(colorSourceCombo_);
    const QSignalBlocker mapBlock(colorMapCombo_);
    colorSourceCombo_->clear();
    for (const PointColorSource source : availablePointColorSources(
             metadata, layer->rasterColors.has_value())) {
        colorSourceCombo_->addItem(pointColorSourceLabel(source),
                                   static_cast<int>(source));
    }
    const int sourceIndex =
        colorSourceCombo_->findData(static_cast<int>(layer->colorMode.source));
    colorSourceCombo_->setCurrentIndex(sourceIndex >= 0 ? sourceIndex : 0);
    colorSourceCombo_->setEnabled(colorSourceCombo_->count() > 1);

    colorMapCombo_->clear();
    for (const PointColorMap map :
         availablePointColorMaps(*colorMaps_, layer->colorMode.source)) {
        addPointColorMapOption(*colorMapCombo_, *colorMaps_, map);
    }
    const int mapIndex =
        colorMapCombo_->findData(static_cast<int>(layer->colorMode.colorMap));
    colorMapCombo_->setCurrentIndex(mapIndex >= 0 ? mapIndex : 0);
    updatePointColorMapToolTip(*colorMapCombo_);
    colorMapCombo_->setEnabled(layer->colorMode.source !=
                                   PointColorSource::Rgb &&
                               colorMapCombo_->count() > 1);

    const bool allCompatible = context_.allCompatible;
    const bool anyDifferent = context_.anyDifferent;
    applyColorToAllButton_->setVisible(context_.pointCount > 1);
    applyColorToAllButton_->setEnabled(context_.pointCount > 1 &&
                                       allCompatible && anyDifferent &&
                                       !layer->rasterColors);
    if (layer->rasterColors) {
        applyColorToAllButton_->setToolTip(
            QStringLiteral("Baked per-point raster colors cannot be applied as "
                           "a layer appearance."));
    } else if (!allCompatible) {
        applyColorToAllButton_->setToolTip(QStringLiteral(
            "This color is not available for every point cloud."));
    } else if (!anyDifferent) {
        applyColorToAllButton_->setToolTip(QStringLiteral(
            "All point clouds already use this color and color map."));
    } else {
        applyColorToAllButton_->setToolTip(QStringLiteral(
            "Use this color and color map for every point cloud."));
    }

    const bool ranged =
        pointColorSourceUsesScalarRange(layer->colorMode.source);
    colorRangeRowLabel_->setVisible(ranged);
    colorRangeWidget_->setVisible(ranged);
    if (!ranged) {
        return;
    }
    const bool integral =
        layer->colorMode.source == PointColorSource::Intensity;
    colorRangeMinimumSpin_->setDecimals(integral ? 0 : 6);
    colorRangeMaximumSpin_->setDecimals(integral ? 0 : 6);
    const Bounds3d colorBounds = context_.colorBounds;
    const std::optional<PointScalarRange> automaticRange =
        automaticPointColorRange(
            layer->colorMode.source, colorBounds, layer->scalarRanges);
    const PointScalarRange displayed =
        layer->colorMode.manualRange
            .or_else([&automaticRange] {
                return automaticRange;
            })
            .value_or(PointScalarRange{0.0, 1.0});
    const double span =
        std::max(displayed.maximum - displayed.minimum, integral ? 1.0 : 1e-6);
    const double step = integral ? 1.0 : span / 100.0;
    const QSignalBlocker autoBlock(automaticColorRangeCheck_);
    const QSignalBlocker minimumBlock(colorRangeMinimumSpin_);
    const QSignalBlocker maximumBlock(colorRangeMaximumSpin_);
    automaticColorRangeCheck_->setChecked(
        !layer->colorMode.manualRange.has_value());
    colorRangeMinimumSpin_->setSingleStep(step);
    colorRangeMaximumSpin_->setSingleStep(step);
    colorRangeMinimumSpin_->setValue(displayed.minimum);
    colorRangeMaximumSpin_->setValue(displayed.maximum);
    colorRangeMinimumSpin_->setEnabled(
        layer->colorMode.manualRange.has_value());
    colorRangeMaximumSpin_->setEnabled(
        layer->colorMode.manualRange.has_value());
}

void PointInspectorPanel::applyColorSource(const int index)
{
    const auto layerId = selectedLayerId();
    if (!layerId || index < 0 ||
        !colorSourceCombo_->itemData(index).isValid()) {
        return;
    }
    const auto source = static_cast<PointColorSource>(
        colorSourceCombo_->itemData(index).toInt());
    const PointColorMode mode{
        .source = source,
        .colorMap = defaultPointColorMap(*colorMaps_, source),
        .manualRange = std::nullopt,
    };

    // Refresh the map choices for the new source without re-firing.
    {
        const QSignalBlocker blocker(colorMapCombo_);
        colorMapCombo_->clear();
        for (const PointColorMap map :
             availablePointColorMaps(*colorMaps_, source)) {
            addPointColorMapOption(*colorMapCombo_, *colorMaps_, map);
        }
        const int mapIndex =
            colorMapCombo_->findData(static_cast<int>(mode.colorMap));
        colorMapCombo_->setCurrentIndex(mapIndex >= 0 ? mapIndex : 0);
        updatePointColorMapToolTip(*colorMapCombo_);
        colorMapCombo_->setEnabled(source != PointColorSource::Rgb &&
                                   colorMapCombo_->count() > 1);
    }

    const std::vector<PointCloudLayerId> selection = selectedLayerIds();
    for (const PointCloudLayerId selectedId : selection) {
        const PointCloudLayerSnapshot *selected = layerById(selectedId);
        if (selected &&
            pointColorModeAvailable(*colorMaps_,
                                    selected->descriptor.metadata,
                                    mode,
                                    selected->rasterColors.has_value())) {
            emit pointColorModeChanged(selectedId, mode);
        }
    }
}

void PointInspectorPanel::applyColorMap(const int index)
{
    const auto layerId = selectedLayerId();
    if (!layerId || index < 0 || !colorMapCombo_->itemData(index).isValid() ||
        colorSourceCombo_->currentIndex() < 0) {
        return;
    }
    const auto source =
        static_cast<PointColorSource>(colorSourceCombo_->currentData().toInt());
    const auto map =
        static_cast<PointColorMap>(colorMapCombo_->itemData(index).toInt());
    const std::vector<PointCloudLayerId> selection = selectedLayerIds();
    for (const PointCloudLayerId selectedId : selection) {
        const PointCloudLayerSnapshot *selected = layerById(selectedId);
        if (!selected) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.source = source;
        mode.colorMap = map;
        if (pointColorModeAvailable(*colorMaps_,
                                    selected->descriptor.metadata,
                                    mode,
                                    selected->rasterColors.has_value())) {
            emit pointColorModeChanged(selectedId, mode);
        }
    }
}

void PointInspectorPanel::applyColorToAll()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayerSnapshot *layer =
        layerId ? layerById(*layerId) : nullptr;
    if (!layer || !applyColorToAllButton_->isEnabled()) {
        return;
    }
    emit allPointColorModesChanged({
        .source = layer->colorMode.source,
        .colorMap = layer->colorMode.colorMap,
        .manualRange = std::nullopt,
    });
}

void PointInspectorPanel::showClassificationFilterDialog()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayerSnapshot *layer =
        layerId ? layerById(*layerId) : nullptr;
    if (!layer || !layer->descriptor.metadata.hasClassification) {
        return;
    }

    auto *dialog = new ClassificationFilterDialog(colorMaps_,
                                                  layer->classificationFilter,
                                                  context_.classifications,
                                                  layerLabel(*layer),
                                                  this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &QDialog::accepted, this, [this, dialog, layerId = *layerId] {
            emit classificationFilterChanged(
                layerId, dialog->filter(), dialog->applyToAllLayers());
        });
    dialog->open();
}

void PointInspectorPanel::applyColorRange()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayerSnapshot *layer =
        layerId ? layerById(*layerId) : nullptr;
    if (!layer || automaticColorRangeCheck_->isChecked() ||
        !pointColorSourceUsesScalarRange(layer->colorMode.source)) {
        return;
    }
    const PointScalarRange range{
        .minimum = colorRangeMinimumSpin_->value(),
        .maximum = colorRangeMaximumSpin_->value(),
    };
    if (!validPointScalarRange(range, false)) {
        return;
    }
    const PointColorSource activeSource = layer->colorMode.source;
    const std::vector<PointCloudLayerId> selection = selectedLayerIds();
    for (const PointCloudLayerId selectedId : selection) {
        const PointCloudLayerSnapshot *selected = layerById(selectedId);
        if (!selected || selected->colorMode.source != activeSource) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.manualRange = range;
        emit pointColorModeChanged(selectedId, mode);
    }
}

void PointInspectorPanel::applyAutomaticColorRange(const bool automatic)
{
    const auto layerId = selectedLayerId();
    const PointCloudLayerSnapshot *layer =
        layerId ? layerById(*layerId) : nullptr;
    if (!layer || !pointColorSourceUsesScalarRange(layer->colorMode.source)) {
        return;
    }
    const PointColorSource activeSource = layer->colorMode.source;
    colorRangeMinimumSpin_->setEnabled(!automatic);
    colorRangeMaximumSpin_->setEnabled(!automatic);
    std::optional<PointScalarRange> manualRange;
    if (!automatic) {
        PointScalarRange range{
            .minimum = colorRangeMinimumSpin_->value(),
            .maximum = colorRangeMaximumSpin_->value(),
        };
        if (!validPointScalarRange(range, false)) {
            const double step =
                layer->colorMode.source == PointColorSource::Intensity ? 1.0
                                                                       : 1e-6;
            range.maximum = range.minimum + step;
            const QSignalBlocker blocker(colorRangeMaximumSpin_);
            colorRangeMaximumSpin_->setValue(range.maximum);
        }
        manualRange = range;
    }
    const std::vector<PointCloudLayerId> selection = selectedLayerIds();
    for (const PointCloudLayerId selectedId : selection) {
        const PointCloudLayerSnapshot *selected = layerById(selectedId);
        if (!selected || selected->colorMode.source != activeSource) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.manualRange = manualRange;
        emit pointColorModeChanged(selectedId, mode);
    }
}

} // namespace pci
