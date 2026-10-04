#pragma once
#include <QObject>
#include <functional>
class QMainWindow;
class QAction;
class QLabel;
namespace pci {
class SceneSession;
class RenderViewport;
class SceneLayersDock;
class LayerInspectorDock;
class TaskDock;
class DiagnosticsDock;
struct WindowActionCallbacks {
    std::function<void()> chooseSources;
    std::function<void()> showSettings;
    std::function<void()> showControlsReference;
    std::function<void()> cancelLoading;
};
class WindowActions final : public QObject {
public:
    WindowActions(QMainWindow &window,
                  SceneSession &session,
                  RenderViewport &viewport,
                  SceneLayersDock &layers,
                  LayerInspectorDock &inspector,
                  TaskDock &tasks,
                  QLabel &navigationHint,
                  WindowActionCallbacks callbacks,
                  DiagnosticsDock *diagnostics = nullptr);
    QAction *newSceneAction_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *fitSceneAction_ = nullptr;
    QAction *topDownSceneAction_ = nullptr;
    QAction *mapViewAction_ = nullptr;
    QAction *orthographicAction_ = nullptr;
    QAction *eyeDomeLightingAction_ = nullptr;
    QAction *navigateToolAction_ = nullptr;
    QAction *measureToolAction_ = nullptr;
    QAction *fitSelectedLayerAction_ = nullptr;
    QAction *isolateSelectedLayerAction_ = nullptr;
    QAction *showAllLayersAction_ = nullptr;
    QAction *removeSelectedLayerAction_ = nullptr;

private:
    void createFileActions();
    void createEditActions();
    void createViewActions();
    void createLayerActions();
    void createToolActions();
    void createHelpActions();
    void createToolbars();
    QMainWindow &window_;
    SceneSession &session_;
    RenderViewport &viewport_;
    SceneLayersDock *sceneLayersDock_;
    LayerInspectorDock *layerInspectorDock_;
    TaskDock *taskDock_;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    DiagnosticsDock *diagnosticsDock_;
#endif
    QLabel *navigationHintLabel_;
    WindowActionCallbacks callbacks_;
    QAction *settingsAction_ = nullptr;
};
} // namespace pci
