#include "InspectorFormatting.h"
#include "PointInspectorPanel.h"
#include "RasterInspectorPanel.h"
#include "VectorInspectorPanel.h"
#include <pci/desktop/ui/LayerInspectorDock.h>
namespace pci {
using namespace inspector;
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

    pointPanel_ = new PointInspectorPanel(inspectorContent, colorMaps_);
    vectorPanel_ = new VectorInspectorPanel(inspectorContent);
    rasterPanel_ = new RasterInspectorPanel(inspectorContent, colorMaps_);
    inspectorLayout->addWidget(pointPanel_);
    inspectorLayout->addWidget(vectorPanel_);
    inspectorLayout->addWidget(rasterPanel_);
    inspectorLayout->addStretch(1);
    inspectorScroll->setWidget(inspectorContent);
    setWidget(inspectorScroll);
    connect(pointPanel_,
            &PointInspectorPanel::pointColorModeChanged,
            this,
            &LayerInspectorDock::pointColorModeChanged);
    connect(pointPanel_,
            &PointInspectorPanel::revertRasterColorsRequested,
            this,
            &LayerInspectorDock::revertRasterColorsRequested);
    connect(pointPanel_,
            &PointInspectorPanel::colorizeFromRasterRequested,
            this,
            &LayerInspectorDock::colorizeFromRasterRequested);
    connect(pointPanel_,
            &PointInspectorPanel::allPointColorModesChanged,
            this,
            &LayerInspectorDock::allPointColorModesChanged);
    connect(pointPanel_,
            &PointInspectorPanel::classificationFilterChanged,
            this,
            &LayerInspectorDock::classificationFilterChanged);
    connect(vectorPanel_,
            &VectorInspectorPanel::vectorStyleChanged,
            this,
            &LayerInspectorDock::vectorStyleChanged);
    connect(vectorPanel_,
            &VectorInspectorPanel::showVectorAnywayRequested,
            this,
            &LayerInspectorDock::showVectorAnywayRequested);
    connect(rasterPanel_,
            &RasterInspectorPanel::rasterStyleChanged,
            this,
            &LayerInspectorDock::rasterStyleChanged);
    connect(rasterPanel_,
            &RasterInspectorPanel::showRasterAnywayRequested,
            this,
            &LayerInspectorDock::showRasterAnywayRequested);
    connect(rasterPanel_,
            &RasterInspectorPanel::retryRasterElevationRequested,
            this,
            &LayerInspectorDock::retryRasterElevationRequested);
    connect(rasterPanel_,
            &RasterInspectorPanel::cancelRasterElevationRequested,
            this,
            &LayerInspectorDock::cancelRasterElevationRequested);
    updateProperties();
}

void LayerInspectorDock::setDocumentSnapshot(SceneDocumentSnapshotPtr snapshot,
                                             SceneLayerId selectedLayerId)
{
    if (!snapshot)
        throw std::invalid_argument("document snapshot must not be null");
    snapshot_ = std::move(snapshot);
    selectedLayerId_ = selectedLayerId;
    updateProperties();
}
void LayerInspectorDock::setColorizeJobState(const bool active,
                                             const bool committing)
{
    if (colorizeJobActive_ == active && colorizeJobCommitting_ == committing) {
        return;
    }
    colorizeJobActive_ = active;
    colorizeJobCommitting_ = committing;
    updateProperties();
}

void LayerInspectorDock::setRasterSurfaceCapability(
    RasterSurfaceCapability capability, QString reason)
{
    rasterPanel_->setSurfaceCapability(capability, std::move(reason));
}
void LayerInspectorDock::updateProperties()
{
    auto point = snapshot_ ? snapshot_->layer(selectedLayerId_) : std::nullopt;
    auto vector =
        snapshot_ ? snapshot_->vectorLayer(selectedLayerId_) : std::nullopt;
    auto raster =
        snapshot_ ? snapshot_->rasterLayer(selectedLayerId_) : std::nullopt;
    InspectorContext context;
    context.colorizeActive = colorizeJobActive_;
    context.colorizeCommitting = colorizeJobCommitting_;
    if (snapshot_) {
        context.pointCount = snapshot_->pointLayers().size();
        context.rasterCount = snapshot_->rasterLayerCount();
        std::optional<Bounds3d> bounds;
        for (const auto &candidate : snapshot_->pointLayers()) {
            const auto sourceBounds =
                candidate.descriptor.metadata.sourceBounds;
            const auto next =
                sourceBounds.valid() ? sourceBounds : candidate.availableBounds;
            if (next.valid()) {
                if (!bounds)
                    bounds = next;
                else
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        bounds->minimum[axis] =
                            std::min(bounds->minimum[axis], next.minimum[axis]);
                        bounds->maximum[axis] =
                            std::max(bounds->maximum[axis], next.maximum[axis]);
                    }
            }
            if (candidate.descriptor.metadata.hasClassification)
                context.classifications.include(
                    candidate.presentClassifications);
            if (point) {
                const PointColorMode sharedMode{
                    .source = point->colorMode.source,
                    .colorMap = point->colorMode.colorMap,
                    .manualRange = std::nullopt};
                context.allCompatible &=
                    pointColorModeAvailable(*colorMaps_,
                                            candidate.descriptor.metadata,
                                            sharedMode,
                                            candidate.rasterColors.has_value());
                context.anyDifferent |=
                    candidate.colorMode.source != sharedMode.source ||
                    candidate.colorMode.colorMap != sharedMode.colorMap;
            }
        }
        context.colorBounds = bounds.value_or(Bounds3d{});
    }
    const bool hasLayers = snapshot_ && snapshot_->hasAnyLayer();
    inspectorTitleLabel_->setText(
        point       ? layerLabel(*point)
        : vector    ? layerLabel(*vector)
        : raster    ? displayPathName(raster->descriptor.metadata.sourcePath)
        : hasLayers ? QStringLiteral("Scene")
                    : QStringLiteral("Empty scene"));
    inspectorTypeLabel_->setText(
        point       ? QStringLiteral("Point-cloud layer")
        : vector    ? QStringLiteral("Planar vector overlay")
        : raster    ? QStringLiteral("Raster layer")
        : hasLayers ? QStringLiteral("No layer selected")
                    : QStringLiteral("No layers"));
    inspectorEmptyLabel_->setVisible(!point && !vector && !raster);
    pointPanel_->present(std::move(point), context);
    vectorPanel_->present(std::move(vector), context);
    rasterPanel_->present(std::move(raster), context);
}
} // namespace pci
