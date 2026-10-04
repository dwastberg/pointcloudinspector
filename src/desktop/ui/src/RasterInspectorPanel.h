#pragma once
#include "InspectorContext.h"
#include <QWidget>
#include <pci/color/PointColorMapCatalog.h>
#include <pci/desktop/viewport/RenderViewport.h>
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
namespace pci {
class VectorColorButton;
class RasterInspectorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit RasterInspectorPanel(QWidget *parent,
                                  PointColorMapCatalogSnapshotPtr colorMaps);
    void present(std::optional<RasterLayerSnapshot> selected,
                 InspectorContext context);
    void setSurfaceCapability(RasterSurfaceCapability value, QString reason)
    {
        rasterSurfaceCapability_ = value;
        rasterSurfaceCapabilityReason_ = std::move(reason);
        updateProperties();
    }
signals:
    void rasterStyleChanged(pci::SceneLayerId layerId,
                            pci::RasterLayerStyle style);
    void showRasterAnywayRequested(pci::SceneLayerId layerId);
    void retryRasterElevationRequested(pci::SceneLayerId layerId);
    void cancelRasterElevationRequested(pci::SceneLayerId layerId);

private:
    void updateProperties();
    void applyRasterStyle();
    const RasterLayerSnapshot *rasterLayerById(SceneLayerId id) const
    {
        return selected_ && selected_->id == id ? &*selected_ : nullptr;
    }
    std::optional<SceneLayerId> selectedLayerId() const
    {
        return selected_ ? std::optional(selected_->id) : std::nullopt;
    }
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

    std::optional<RasterLayerSnapshot> selected_;
    InspectorContext context_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    RasterSurfaceCapability rasterSurfaceCapability_ =
        RasterSurfaceCapability::Unknown;
    QString rasterSurfaceCapabilityReason_;
};
} // namespace pci
