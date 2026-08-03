#include "app/LayerInspectorDock.h"

#include "app/ClassificationFilterDialog.h"
#include "app/VectorColorButton.h"
#include "platform/QtPath.h"
#include "renderer/PointColorLabels.h"

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
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

QString layerLabel(const PointCloudLayer &layer)
{
    const std::filesystem::path &path = layer.scene->metadata().sourcePath;
    return path.empty() ? QStringLiteral("Point cloud %1").arg(layer.id.value())
                        : displayPathName(path);
}

QString layerLabel(const VectorLayer &layer)
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

QString attributesText(const PointCloudMetadata &metadata)
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

QString boundsText(const Bounds3d &bounds)
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

QLabel *makeValueLabel()
{
    auto *label = new ElidedLabel();
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setText(QStringLiteral("—"));
    return label;
}

void setValueText(QLabel *label, const QString &text)
{
    label->setText(text);
    label->setToolTip(text);
}

Bounds3d documentColorBounds(const std::vector<PointCloudLayer> &layers)
{
    std::optional<Bounds3d> combined;
    for (const PointCloudLayer &layer : layers) {
        const Bounds3d sourceBounds = layer.scene->metadata().sourceBounds;
        const Bounds3d bounds =
            sourceBounds.valid() ? sourceBounds : layer.scene->bounds();
        if (!bounds.valid()) {
            continue;
        }
        if (!combined) {
            combined = bounds;
            continue;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            combined->minimum[axis] =
                std::min(combined->minimum[axis], bounds.minimum[axis]);
            combined->maximum[axis] =
                std::max(combined->maximum[axis], bounds.maximum[axis]);
        }
    }
    return combined.value_or(Bounds3d{});
}

PointClassificationFilter
documentClassifications(const std::vector<PointCloudLayer> &layers)
{
    PointClassificationFilter result = PointClassificationFilter::noneVisible();
    for (const PointCloudLayer &layer : layers) {
        if (layer.scene->metadata().hasClassification) {
            result.include(layer.scene->presentClassifications());
        }
    }
    return result;
}

std::size_t
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

} // namespace

LayerInspectorDock::LayerInspectorDock(
    QWidget *parent, PointColorMapCatalogSnapshotPtr colorMaps)
    : QDockWidget(QStringLiteral("Inspector"), parent)
    , colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
{
    setObjectName(QStringLiteral("pointCloudInspectorPanel"));
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(260);

    auto *inspectorScroll = new QScrollArea(this);
    inspectorScroll->setObjectName(QStringLiteral("pointCloudInspectorScroll"));
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    auto *inspectorContent = new QWidget(inspectorScroll);
    auto *inspectorLayout = new QVBoxLayout(inspectorContent);
    inspectorLayout->setContentsMargins(12, 12, 12, 12);
    inspectorLayout->setSpacing(12);

    inspectorTitleLabel_ =
        new QLabel(QStringLiteral("Scene"), inspectorContent);
    inspectorTitleLabel_->setObjectName(
        QStringLiteral("pointCloudInspectorTitle"));
    QFont titleFont = inspectorTitleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    inspectorTitleLabel_->setFont(titleFont);
    inspectorTitleLabel_->setWordWrap(true);
    inspectorLayout->addWidget(inspectorTitleLabel_);
    inspectorTypeLabel_ =
        new QLabel(QStringLiteral("No layer selected"), inspectorContent);
    inspectorTypeLabel_->setObjectName(
        QStringLiteral("pointCloudInspectorType"));
    inspectorTypeLabel_->setEnabled(false);
    inspectorLayout->addWidget(inspectorTypeLabel_);

    inspectorEmptyLabel_ = new QLabel(
        QStringLiteral(
            "Select a layer in the Scene panel to inspect and edit it."),
        inspectorContent);
    inspectorEmptyLabel_->setObjectName(
        QStringLiteral("pointCloudInspectorEmptyLabel"));
    inspectorEmptyLabel_->setWordWrap(true);
    inspectorEmptyLabel_->setEnabled(false);
    inspectorLayout->addWidget(inspectorEmptyLabel_);

    propertiesWidget_ = new QWidget(inspectorContent);
    propertiesWidget_->setObjectName(
        QStringLiteral("pointCloudLayerProperties"));
    auto *propertiesLayout = new QVBoxLayout(propertiesWidget_);
    propertiesLayout->setContentsMargins(0, 0, 0, 0);
    propertiesLayout->setSpacing(12);

    auto *appearanceGroup =
        new QGroupBox(QStringLiteral("Appearance"), propertiesWidget_);
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
    appearanceForm->addRow(colorRangeRowLabel_, colorRangeWidget_);
    appearanceForm->addRow(applyColorToAllButton_);
    propertiesLayout->addWidget(appearanceGroup);

    auto *filtersGroup =
        new QGroupBox(QStringLiteral("Filters"), propertiesWidget_);
    filtersGroup->setObjectName(QStringLiteral("pointCloudFiltersSection"));
    auto *filtersLayout = new QVBoxLayout(filtersGroup);
    classificationFilterButton_ = new QPushButton(
        QStringLiteral("Filter classifications…"), filtersGroup);
    classificationFilterButton_->setObjectName(
        QStringLiteral("classificationFilterButton"));
    filtersLayout->addWidget(classificationFilterButton_);
    propertiesLayout->addWidget(filtersGroup);

    auto *informationGroup =
        new QGroupBox(QStringLiteral("Information"), propertiesWidget_);
    informationGroup->setObjectName(
        QStringLiteral("pointCloudInformationSection"));
    auto *informationForm = new QFormLayout(informationGroup);
    informationForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    informationForm->addRow(QStringLiteral("Points"), pointsValue_);
    informationForm->addRow(QStringLiteral("Attributes"), attributesValue_);
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    informationForm->addRow(QStringLiteral("Local index"), indexValue_);
#endif
    informationForm->addRow(QStringLiteral("Size (X×Y×Z)"), boundsValue_);
    informationForm->addRow(QStringLiteral("CRS"), crsValue_);
    informationForm->addRow(QStringLiteral("Source"), sourceValue_);
    propertiesLayout->addWidget(informationGroup);
    inspectorLayout->addWidget(propertiesWidget_);

    vectorPropertiesWidget_ = new QWidget(inspectorContent);
    vectorPropertiesWidget_->setObjectName(
        QStringLiteral("vectorLayerProperties"));
    auto *vectorLayout = new QVBoxLayout(vectorPropertiesWidget_);
    vectorLayout->setContentsMargins(0, 0, 0, 0);
    vectorLayout->setSpacing(12);

    vectorVisibilityWarning_ = new QWidget(vectorPropertiesWidget_);
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

    auto *vectorAppearance =
        new QGroupBox(QStringLiteral("Appearance"), vectorPropertiesWidget_);
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

    auto *placement =
        new QGroupBox(QStringLiteral("Placement"), vectorPropertiesWidget_);
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
        new QGroupBox(QStringLiteral("Information"), vectorPropertiesWidget_);
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

    inspectorLayout->addWidget(vectorPropertiesWidget_);
    inspectorLayout->addStretch(1);
    inspectorScroll->setWidget(inspectorContent);
    setWidget(inspectorScroll);

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
                applyColorMap(index);
            });
    connect(applyColorToAllButton_, &QPushButton::clicked, this, [this] {
        applyColorToAll();
    });
    connect(classificationFilterButton_, &QPushButton::clicked, this, [this] {
        showClassificationFilterDialog();
    });
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
        const VectorLayer *vector = id ? vectorLayerById(*id) : nullptr;
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
        const Bounds3d sceneBounds = documentColorBounds(layers_);
        if (!sceneBounds.valid()) {
            return;
        }
        const double height = sceneBounds.maximum[2] - sceneBounds.minimum[2];
        vectorZOffset_->setValue(sceneBounds.maximum[2] +
                                 std::max(height * 0.01, 0.01));
    });
    connect(vectorMatchSceneFloor_, &QPushButton::clicked, this, [this] {
        const Bounds3d sceneBounds = documentColorBounds(layers_);
        if (sceneBounds.valid()) {
            vectorZOffset_->setValue(sceneBounds.minimum[2]);
        }
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

    updateProperties();
}

void LayerInspectorDock::setDocumentSnapshot(SceneDocumentSnapshotPtr snapshot,
                                             const SceneLayerId selectedLayerId)
{
    if (!snapshot) {
        throw std::invalid_argument("document snapshot must not be null");
    }
    layers_ = snapshot->pointLayers();
    vectorLayers_ = snapshot->vectorLayers();
    selectedLayerId_ = selectedLayerId;
    if (!layerById(selectedLayerId_) && !vectorLayerById(selectedLayerId_)) {
        selectedLayerId_ = SceneLayerId{};
    }
    updateProperties();
}

void LayerInspectorDock::applyColorSource(const int index)
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
        .colorMap = defaultPointColorMap(source),
    };

    // Refresh the map choices for the new source without re-firing.
    {
        const QSignalBlocker blocker(colorMapCombo_);
        colorMapCombo_->clear();
        for (const PointColorMap map :
             availablePointColorMaps(*colorMaps_, source)) {
            colorMapCombo_->addItem(pointColorMapLabel(*colorMaps_, map),
                                    static_cast<int>(map));
        }
        const int mapIndex =
            colorMapCombo_->findData(static_cast<int>(mode.colorMap));
        colorMapCombo_->setCurrentIndex(mapIndex >= 0 ? mapIndex : 0);
        colorMapCombo_->setEnabled(source != PointColorSource::Rgb &&
                                   colorMapCombo_->count() > 1);
    }

    const std::vector<PointCloudLayerId> selection = selectedLayerIds();
    for (const PointCloudLayerId selectedId : selection) {
        const PointCloudLayer *selected = layerById(selectedId);
        if (selected && pointColorModeAvailable(
                            *colorMaps_, selected->scene->metadata(), mode)) {
            emit pointColorModeChanged(selectedId, mode);
        }
    }
}

void LayerInspectorDock::applyColorMap(const int index)
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
        const PointCloudLayer *selected = layerById(selectedId);
        if (!selected) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.source = source;
        mode.colorMap = map;
        if (pointColorModeAvailable(
                *colorMaps_, selected->scene->metadata(), mode)) {
            emit pointColorModeChanged(selectedId, mode);
        }
    }
}

void LayerInspectorDock::applyColorToAll()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayer *layer = layerId ? layerById(*layerId) : nullptr;
    if (!layer || !applyColorToAllButton_->isEnabled()) {
        return;
    }
    emit allPointColorModesChanged({
        .source = layer->colorMode.source,
        .colorMap = layer->colorMode.colorMap,
    });
}

void LayerInspectorDock::showClassificationFilterDialog()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayer *layer = layerId ? layerById(*layerId) : nullptr;
    if (!layer || !layer->scene->metadata().hasClassification) {
        return;
    }

    auto *dialog =
        new ClassificationFilterDialog(colorMaps_,
                                       layer->classificationFilter,
                                       documentClassifications(layers_),
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

void LayerInspectorDock::applyColorRange()
{
    const auto layerId = selectedLayerId();
    const PointCloudLayer *layer = layerId ? layerById(*layerId) : nullptr;
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
        const PointCloudLayer *selected = layerById(selectedId);
        if (!selected || selected->colorMode.source != activeSource) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.manualRange = range;
        emit pointColorModeChanged(selectedId, mode);
    }
}

void LayerInspectorDock::applyAutomaticColorRange(const bool automatic)
{
    const auto layerId = selectedLayerId();
    const PointCloudLayer *layer = layerId ? layerById(*layerId) : nullptr;
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
        const PointCloudLayer *selected = layerById(selectedId);
        if (!selected || selected->colorMode.source != activeSource) {
            continue;
        }
        PointColorMode mode = selected->colorMode;
        mode.manualRange = manualRange;
        emit pointColorModeChanged(selectedId, mode);
    }
}

void LayerInspectorDock::updateProperties()
{
    const bool hasLayers = !layers_.empty() || !vectorLayers_.empty();

    const auto layerId = selectedLayerId();
    const PointCloudLayer *layer = layerId ? layerById(*layerId) : nullptr;
    const VectorLayer *vector = layerId ? vectorLayerById(*layerId) : nullptr;
    propertiesWidget_->setVisible(layer != nullptr);
    vectorPropertiesWidget_->setVisible(vector != nullptr);
    inspectorEmptyLabel_->setVisible(layer == nullptr && vector == nullptr);
    if (!layer) {
        if (vector) {
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
            const bool hiddenDisjoint = vector->data &&
                                        vector->data->extentDisjointXY &&
                                        !vector->visible;
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
            vectorVisibilityWarningLabel_->setText(
                warnings.join(QStringLiteral("\n")));
            vectorShowAnywayButton_->setVisible(hiddenDisjoint);

            const Bounds3d sceneBounds = documentColorBounds(layers_);
            vectorPlaceAboveScene_->setEnabled(sceneBounds.valid());
            vectorMatchSceneFloor_->setEnabled(sceneBounds.valid());
            QString placementText = QStringLiteral(
                "Uses one constant elevation; the overlay does not "
                "follow terrain.");
            if (sceneBounds.valid() && vector->data &&
                vector->data->bounds.valid()) {
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
                            .arg(sceneBounds.minimum[2] - placedMaximum,
                                 0,
                                 'f',
                                 2)
                            .arg(sceneBounds.minimum[2], 0, 'f', 2)
                            .arg(sceneBounds.maximum[2], 0, 'f', 2);
                } else if (placedMinimum > sceneBounds.maximum[2]) {
                    placementText =
                        QStringLiteral(
                            "The overlay is %1 above the point-cloud height "
                            "range (%2 to %3).")
                            .arg(placedMinimum - sceneBounds.maximum[2],
                                 0,
                                 'f',
                                 2)
                            .arg(sceneBounds.minimum[2], 0, 'f', 2)
                            .arg(sceneBounds.maximum[2], 0, 'f', 2);
                } else {
                    placementText =
                        QStringLiteral(
                            "Planar elevation intersects the point-cloud "
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
                             locale.toString(static_cast<qulonglong>(
                                 vector->data->featureCount)));
                setValueText(vectorGeometryValue_,
                             QString::fromStdString(
                                 vector->data->geometryDescription()));
                setValueText(
                    vectorDriverValue_,
                    vector->data->sourceDriver.empty()
                        ? QStringLiteral("—")
                        : QString::fromStdString(vector->data->sourceDriver));
                setValueText(vectorCrsValue_,
                             vector->data->spatialReferenceWkt.empty()
                                 ? QStringLiteral("Not reported")
                                 : QString::fromStdString(
                                       vector->data->spatialReferenceWkt));
                setValueText(vectorBoundsValue_,
                             boundsText(vector->data->bounds));
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
        inspectorTitleLabel_->setText(vector ? layerLabel(*vector)
                                      : hasLayers
                                          ? QStringLiteral("Scene")
                                          : QStringLiteral("Empty scene"));
        inspectorTypeLabel_->setText(
            vector      ? QStringLiteral("Planar vector overlay")
            : hasLayers ? QStringLiteral("No layer selected")
                        : QStringLiteral("No layers"));
        return;
    }

    constexpr int selectedCount = 1;
    inspectorTitleLabel_->setText(
        selectedCount > 1
            ? QStringLiteral("%1 layers selected").arg(selectedCount)
            : layerLabel(*layer));
    inspectorTypeLabel_->setText(
        selectedCount > 1
            ? QStringLiteral(
                  "Point-cloud layers · edits apply to compatible selections")
            : QStringLiteral("Point-cloud layer"));

    const PointCloudMetadata &metadata = layer->scene->metadata();
    const QLocale locale;
    const std::uint64_t resident = layer->scene->totalPointCount();
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

    setValueText(boundsValue_, boundsText(layer->scene->bounds()));
    setValueText(attributesValue_, attributesText(metadata));
    classificationFilterButton_->setVisible(metadata.hasClassification);
    if (classificationFilterButton_->parentWidget()) {
        classificationFilterButton_->parentWidget()->setVisible(
            metadata.hasClassification);
    }
    if (metadata.hasClassification) {
        const PointClassificationFilter available =
            documentClassifications(layers_);
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
    const PointCloudStorageMetrics storage = layer->scene->storageMetrics();
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
    for (const PointColorSource source : availablePointColorSources(metadata)) {
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
        colorMapCombo_->addItem(pointColorMapLabel(*colorMaps_, map),
                                static_cast<int>(map));
    }
    const int mapIndex =
        colorMapCombo_->findData(static_cast<int>(layer->colorMode.colorMap));
    colorMapCombo_->setCurrentIndex(mapIndex >= 0 ? mapIndex : 0);
    colorMapCombo_->setEnabled(layer->colorMode.source !=
                                   PointColorSource::Rgb &&
                               colorMapCombo_->count() > 1);

    const PointColorMode sharedColorMode{
        .source = layer->colorMode.source,
        .colorMap = layer->colorMode.colorMap,
    };
    const bool allCompatible = std::ranges::all_of(
        layers_, [this, &sharedColorMode](const PointCloudLayer &candidate) {
            return pointColorModeAvailable(
                *colorMaps_, candidate.scene->metadata(), sharedColorMode);
        });
    const bool anyDifferent = std::ranges::any_of(
        layers_, [&sharedColorMode](const PointCloudLayer &candidate) {
            return candidate.colorMode.source != sharedColorMode.source ||
                   candidate.colorMode.colorMap != sharedColorMode.colorMap;
        });
    applyColorToAllButton_->setVisible(layers_.size() > 1);
    applyColorToAllButton_->setEnabled(layers_.size() > 1 && allCompatible &&
                                       anyDifferent);
    if (!allCompatible) {
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
    const Bounds3d colorBounds = documentColorBounds(layers_);
    const std::optional<PointScalarRange> automaticRange =
        automaticPointColorRange(
            layer->colorMode.source, colorBounds, layer->scene->scalarRanges());
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

void LayerInspectorDock::applyVectorStyle()
{
    const auto layerId = selectedLayerId();
    const VectorLayer *layer = layerId ? vectorLayerById(*layerId) : nullptr;
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

std::optional<PointCloudLayerId> LayerInspectorDock::selectedLayerId() const
{
    if (selectedLayerId_.value() == 0) {
        return std::nullopt;
    }
    return selectedLayerId_;
}

std::vector<PointCloudLayerId> LayerInspectorDock::selectedLayerIds() const
{
    if (const auto selected = selectedLayerId();
        selected && layerById(*selected)) {
        return {*selected};
    }
    return {};
}

const PointCloudLayer *
LayerInspectorDock::layerById(const PointCloudLayerId id) const
{
    for (const PointCloudLayer &layer : layers_) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

const VectorLayer *
LayerInspectorDock::vectorLayerById(const SceneLayerId id) const
{
    for (const VectorLayer &layer : vectorLayers_) {
        if (layer.id == id)
            return &layer;
    }
    return nullptr;
}

} // namespace pci
