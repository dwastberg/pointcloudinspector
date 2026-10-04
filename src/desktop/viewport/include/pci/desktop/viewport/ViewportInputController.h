#pragma once

#include <QPoint>
#include <pci/navigation/NavigationCamera.h>
#include <pci/navigation/NavigationInputState.h>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QFocusEvent;

namespace pci {
class RenderViewportWidget;
class ViewportInputController final {
public:
    explicit ViewportInputController(RenderViewportWidget &viewport)
        : viewport_(viewport)
    {
    }
    bool mousePressEvent(QMouseEvent *event);
    bool mouseDoubleClickEvent(QMouseEvent *event);
    bool mouseMoveEvent(QMouseEvent *event);
    bool mouseReleaseEvent(QMouseEvent *event);
    bool wheelEvent(QWheelEvent *event);
    bool keyPressEvent(QKeyEvent *event);
    bool keyReleaseEvent(QKeyEvent *event);
    bool focusOutEvent(QFocusEvent *event);
    NavigationCamera &camera() noexcept
    {
        return camera_;
    }
    NavigationInputState &state() noexcept
    {
        return state_;
    }
    bool dragging() const noexcept
    {
        return dragMode_ != DragMode::None;
    }
    void resetGesture() noexcept
    {
        dragMode_ = DragMode::None;
        pendingMeasureClick_ = false;
    }

private:
    enum class DragMode {
        None,
        Orbit,
        Pan
    };
    RenderViewportWidget &viewport_;
    NavigationCamera camera_;
    NavigationInputState state_;
    DragMode dragMode_ = DragMode::None;
    QPoint previousMousePosition_;
    QPoint leftPressPosition_;
    bool pendingMeasureClick_ = false;
    bool suppressMeasureRelease_ = false;
};
} // namespace pci
