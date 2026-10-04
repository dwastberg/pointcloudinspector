#pragma once

#include <pci/desktop/ui/SceneLayerMetaType.h>

#include <pci/pointcloud/PointColorPolicy.h>

#include <pci/color/PointColorMapCatalog.h>
#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/document/SceneDocumentSnapshot.h>

#include <QDockWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace pci {

class PointInspectorPanel;
class VectorInspectorPanel;
class RasterInspectorPanel;

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
    void updateProperties();
    QLabel *inspectorTitleLabel_ = nullptr;
    QLabel *inspectorTypeLabel_ = nullptr;
    QLabel *inspectorEmptyLabel_ = nullptr;
    PointInspectorPanel *pointPanel_ = nullptr;
    VectorInspectorPanel *vectorPanel_ = nullptr;
    RasterInspectorPanel *rasterPanel_ = nullptr;
    SceneDocumentSnapshotPtr snapshot_;
    SceneLayerId selectedLayerId_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    bool colorizeJobActive_ = false;
    bool colorizeJobCommitting_ = false;
};
} // namespace pci
