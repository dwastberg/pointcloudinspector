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
class PointInspectorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit PointInspectorPanel(QWidget *parent,
                                 PointColorMapCatalogSnapshotPtr colorMaps);
    void present(std::optional<PointCloudLayerSnapshot> selected,
                 InspectorContext context);
signals:
    void pointColorModeChanged(pci::SceneLayerId layerId,
                               pci::PointColorMode mode);
    void revertRasterColorsRequested(pci::SceneLayerId layerId);
    void colorizeFromRasterRequested(pci::SceneLayerId layerId);
    void allPointColorModesChanged(pci::PointColorMode mode);
    void classificationFilterChanged(pci::SceneLayerId layerId,
                                     pci::PointClassificationFilter filter,
                                     bool applyToAllLayers);

private:
    void updateProperties();
    void applyColorSource(int index);
    void applyColorMap(int index);
    void applyColorToAll();
    void showClassificationFilterDialog();
    void applyColorRange();
    void applyAutomaticColorRange(bool automatic);
    const PointCloudLayerSnapshot *layerById(SceneLayerId id) const
    {
        return selected_ && selected_->id == id ? &*selected_ : nullptr;
    }
    std::optional<SceneLayerId> selectedLayerId() const
    {
        return selected_ ? std::optional(selected_->id) : std::nullopt;
    }
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
    std::optional<PointCloudLayerSnapshot> selected_;
    InspectorContext context_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    std::vector<PointCloudLayerId> selectedLayerIds() const
    {
        return selected_ ? std::vector<PointCloudLayerId>{selected_->id}
                         : std::vector<PointCloudLayerId>{};
    }
};
} // namespace pci
