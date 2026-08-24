#pragma once

#include "app/SceneLayerMetaType.h"
#include "scene/SceneDocumentSnapshot.h"

#include <QDockWidget>

#include <optional>

class QAction;
class QListView;
class QPoint;

namespace pci {

class SceneLayerListModel;

class SceneLayersDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit SceneLayersDock(QWidget *parent = nullptr);

    void setDocumentSnapshot(SceneDocumentSnapshotPtr snapshot);
    [[nodiscard]] std::optional<SceneLayerId> currentLayerId() const;
    [[nodiscard]] QAction *statisticsAction() const noexcept;
    [[nodiscard]] QAction *colorizeAction() const noexcept;
    [[nodiscard]] QAction *revertColorsAction() const noexcept;
    void setColorizeJobState(bool active, bool committing);
    void selectAllLayers();
    void fitCurrentLayer();
    void isolateCurrentLayer();
    void removeCurrentLayer();
    void showCurrentLayerStatistics();
    void showAll();

signals:
    void selectionChanged(pci::SceneLayerId layerId);
    void visibilityToggled(pci::SceneLayerId layerId, bool visible);
    void fitRequested(pci::SceneLayerId layerId);
    void isolateRequested(pci::SceneLayerId layerId);
    void removeRequested(pci::SceneLayerId layerId);
    void statisticsRequested(pci::SceneLayerId layerId);
    void colorizeRequested(pci::SceneLayerId layerId);
    void revertColorsRequested(pci::SceneLayerId layerId);
    void showAllRequested();
    void showVectorAnywayRequested(pci::SceneLayerId layerId);
    void addLayerRequested();

private:
    void publishSelection();
    void showContextMenu(const QPoint &position);
    [[nodiscard]] bool currentLayerIsPoint() const;

    QListView *list_ = nullptr;
    SceneLayerListModel *model_ = nullptr;
    QAction *statisticsAction_ = nullptr;
    QAction *colorizeAction_ = nullptr;
    QAction *revertColorsAction_ = nullptr;
    SceneDocumentSnapshotPtr snapshot_;
    bool colorizeJobActive_ = false;
    bool colorizeJobCommitting_ = false;
};

} // namespace pci
