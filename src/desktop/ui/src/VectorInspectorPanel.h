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
class VectorInspectorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit VectorInspectorPanel(QWidget *parent);
    void present(std::optional<VectorLayerSnapshot> selected,
                 InspectorContext context);
signals:
    void vectorStyleChanged(pci::SceneLayerId layerId,
                            pci::VectorLayerStyle style);
    void showVectorAnywayRequested(pci::SceneLayerId layerId);

private:
    void updateProperties();
    void applyVectorStyle();
    const VectorLayerSnapshot *vectorLayerById(SceneLayerId id) const
    {
        return selected_ && selected_->id == id ? &*selected_ : nullptr;
    }
    std::optional<SceneLayerId> selectedLayerId() const
    {
        return selected_ ? std::optional(selected_->id) : std::nullopt;
    }
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
    std::optional<VectorLayerSnapshot> selected_;
    InspectorContext context_;
};
} // namespace pci
