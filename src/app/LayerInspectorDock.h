#pragma once

#include "app/SceneLayerMetaType.h"

#include "pointcloud/PointColorMapCatalog.h"
#include "pointcloud/PointColorPolicy.h"
#include "renderer/RenderViewport.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QDockWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace pci {

class VectorColorButton;

class LayerInspectorDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit LayerInspectorDock(QWidget *parent = nullptr,
                                PointColorMapCatalogSnapshotPtr colorMaps = {});

    void setDocumentSnapshot(SceneDocumentSnapshotPtr snapshot,
                             SceneLayerId selectedLayerId);
    void setColorizeJobState(bool active, bool committing);
    void setRasterSurfaceCapability(RasterSurfaceCapability capability,
                                    QString reason = {});

signals:
    void pointColorModeChanged(pci::SceneLayerId layerId,
                               pci::PointColorMode mode);
    void revertRasterColorsRequested(pci::SceneLayerId layerId);
    void colorizeFromRasterRequested(pci::SceneLayerId layerId);
    void allPointColorModesChanged(pci::PointColorMode mode);
    void classificationFilterChanged(pci::SceneLayerId layerId,
                                     pci::PointClassificationFilter filter,
                                     bool applyToAllLayers);
    void vectorStyleChanged(pci::SceneLayerId layerId,
                            pci::VectorLayerStyle style);
    void showVectorAnywayRequested(pci::SceneLayerId layerId);
    void rasterStyleChanged(pci::SceneLayerId layerId,
                            pci::RasterLayerStyle style);
    void showRasterAnywayRequested(pci::SceneLayerId layerId);
    void retryRasterElevationRequested(pci::SceneLayerId layerId);
    void cancelRasterElevationRequested(pci::SceneLayerId layerId);

private:
    void applyColorSource(int index);
    void applyColorMap(int index);
    void applyColorToAll();
    void showClassificationFilterDialog();
    void applyColorRange();
    void applyAutomaticColorRange(bool automatic);
    void applyVectorStyle();
    void applyRasterStyle();
    void updateRasterProperties(const RasterLayer &raster);
    void updateProperties();
    [[nodiscard]] std::vector<PointCloudLayerId> selectedLayerIds() const;
    [[nodiscard]] std::optional<PointCloudLayerId> selectedLayerId() const;
    [[nodiscard]] const PointCloudLayer *layerById(PointCloudLayerId id) const;
    [[nodiscard]] const VectorLayer *vectorLayerById(SceneLayerId id) const;
    [[nodiscard]] const RasterLayer *rasterLayerById(SceneLayerId id) const;

    QWidget *propertiesWidget_ = nullptr;
    QWidget *vectorPropertiesWidget_ = nullptr;
    QWidget *rasterPropertiesWidget_ = nullptr;
    QLabel *inspectorTitleLabel_ = nullptr;
    QLabel *inspectorTypeLabel_ = nullptr;
    QLabel *inspectorEmptyLabel_ = nullptr;
    QLabel *pointsValue_ = nullptr;
    QLabel *sourceValue_ = nullptr;
    QLabel *crsValue_ = nullptr;
    QLabel *boundsValue_ = nullptr;
    QLabel *attributesValue_ = nullptr;
    QLabel *rasterColorStateValue_ = nullptr;
    QPushButton *colorizeFromRasterButton_ = nullptr;
    QPushButton *revertRasterColorsButton_ = nullptr;
    QLabel *rasterColorsValue_ = nullptr;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    QLabel *indexValue_ = nullptr;
#endif
    QComboBox *colorSourceCombo_ = nullptr;
    QComboBox *colorMapCombo_ = nullptr;
    QPushButton *applyColorToAllButton_ = nullptr;
    QPushButton *classificationFilterButton_ = nullptr;
    QLabel *colorRangeRowLabel_ = nullptr;
    QWidget *colorRangeWidget_ = nullptr;
    QCheckBox *automaticColorRangeCheck_ = nullptr;
    QDoubleSpinBox *colorRangeMinimumSpin_ = nullptr;
    QDoubleSpinBox *colorRangeMaximumSpin_ = nullptr;
    VectorColorButton *vectorFillColor_ = nullptr;
    VectorColorButton *vectorStrokeColor_ = nullptr;
    VectorColorButton *vectorMarkerColor_ = nullptr;
    QDoubleSpinBox *vectorStrokeWidth_ = nullptr;
    QDoubleSpinBox *vectorMarkerSize_ = nullptr;
    QComboBox *vectorMarkerShape_ = nullptr;
    QDoubleSpinBox *vectorOpacity_ = nullptr;
    QPushButton *vectorResetStyle_ = nullptr;
    QDoubleSpinBox *vectorZOffset_ = nullptr;
    QPushButton *vectorPlaceAboveScene_ = nullptr;
    QPushButton *vectorMatchSceneFloor_ = nullptr;
    QCheckBox *vectorAlwaysOnTop_ = nullptr;
    QWidget *vectorVisibilityWarning_ = nullptr;
    QLabel *vectorVisibilityWarningLabel_ = nullptr;
    QPushButton *vectorShowAnywayButton_ = nullptr;
    QLabel *vectorPlacementNotice_ = nullptr;
    QLabel *vectorFeaturesValue_ = nullptr;
    QLabel *vectorGeometryValue_ = nullptr;
    QLabel *vectorSourceValue_ = nullptr;
    QLabel *vectorDriverValue_ = nullptr;
    QLabel *vectorCrsValue_ = nullptr;
    QLabel *vectorBoundsValue_ = nullptr;
    QDoubleSpinBox *rasterOpacity_ = nullptr;
    QWidget *rasterRenderingWidget_ = nullptr;
    QComboBox *rasterRenderMode_ = nullptr;
    QDoubleSpinBox *rasterVerticalExaggeration_ = nullptr;
    QDoubleSpinBox *rasterSurfaceShading_ = nullptr;
    QLabel *rasterElevationStatus_ = nullptr;
    QPushButton *rasterElevationRetry_ = nullptr;
    QPushButton *rasterElevationCancel_ = nullptr;
    QDoubleSpinBox *rasterZOffset_ = nullptr;
    QPushButton *rasterPlaceAboveScene_ = nullptr;
    QPushButton *rasterMatchSceneFloor_ = nullptr;
    QPushButton *rasterResetElevation_ = nullptr;
    QWidget *rasterRangeWidget_ = nullptr;
    QDoubleSpinBox *rasterRangeMinimum_ = nullptr;
    QDoubleSpinBox *rasterRangeMaximum_ = nullptr;
    QComboBox *rasterColorRamp_ = nullptr;
    QWidget *rasterVisibilityWarning_ = nullptr;
    QLabel *rasterVisibilityWarningLabel_ = nullptr;
    QPushButton *rasterShowAnywayButton_ = nullptr;
    QLabel *rasterDimensionsValue_ = nullptr;
    QLabel *rasterBandsValue_ = nullptr;
    QLabel *rasterPixelSizeValue_ = nullptr;
    QLabel *rasterOverviewsValue_ = nullptr;
    QLabel *rasterRangeValue_ = nullptr;
    QLabel *rasterDriverValue_ = nullptr;
    QLabel *rasterCrsValue_ = nullptr;
    QLabel *rasterBoundsValue_ = nullptr;
    QLabel *rasterSourceValue_ = nullptr;

    std::vector<PointCloudLayer> layers_;
    std::vector<VectorLayer> vectorLayers_;
    std::vector<RasterLayer> rasterLayers_;
    SceneLayerId selectedLayerId_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    bool colorizeJobActive_ = false;
    bool colorizeJobCommitting_ = false;
    RasterSurfaceCapability rasterSurfaceCapability_ =
        RasterSurfaceCapability::Unknown;
    QString rasterSurfaceCapabilityReason_;
};

} // namespace pci
