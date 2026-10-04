#include <pci/desktop/viewport/RenderViewportWidget_p.h>
#include <pci/desktop/viewport/ViewportInputController.h>

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace pci {

bool ViewportInputController::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (viewport_.activeTool_ == ViewportTool::Measure) {
            leftPressPosition_ = event->position().toPoint();
            previousMousePosition_ = leftPressPosition_;
            pendingMeasureClick_ = true;
            event->accept();
            return true;
        }
        dragMode_ = viewport_.mapView_ ? DragMode::Pan : DragMode::Orbit;
        previousMousePosition_ = event->position().toPoint();
        event->accept();
        return true;
    }
    if (event->button() == Qt::RightButton) {
        dragMode_ = DragMode::Pan;
        previousMousePosition_ = event->position().toPoint();
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (viewport_.activeTool_ == ViewportTool::Measure) {
            suppressMeasureRelease_ = true;
            pendingMeasureClick_ = false;
            event->accept();
            return true;
        }
        dragMode_ = DragMode::None;
        const QPoint position = event->position().toPoint();
        state_.queuePivot({position.x(), position.y()});
        viewport_.requestRender();
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::mouseMoveEvent(QMouseEvent *event)
{
    if (pendingMeasureClick_) {
        const QPoint position = event->position().toPoint();
        if ((position - leftPressPosition_).manhattanLength() >=
            QApplication::startDragDistance()) {
            pendingMeasureClick_ = false;
            dragMode_ = viewport_.mapView_ ? DragMode::Pan : DragMode::Orbit;
        }
    }
    if (dragMode_ != DragMode::None) {
        const QPoint position = event->position().toPoint();
        const QPoint delta = position - previousMousePosition_;
        previousMousePosition_ = position;
        if (dragMode_ == DragMode::Orbit) {
            camera_.orbitFromDrag(static_cast<float>(delta.x()),
                                  static_cast<float>(delta.y()));
        } else {
            camera_.panFromDrag(static_cast<float>(delta.x()),
                                static_cast<float>(delta.y()),
                                static_cast<float>(viewport_.height()));
        }
        viewport_.requestRender();
        event->accept();
        return true;
    }
    if (viewport_.activeTool_ == ViewportTool::Measure) {
        viewport_.scheduleMeasurementHover(event->position().toPoint());
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton &&
        viewport_.activeTool_ == ViewportTool::Measure) {
        if (suppressMeasureRelease_) {
            suppressMeasureRelease_ = false;
            pendingMeasureClick_ = false;
            event->accept();
            return true;
        }
        if (pendingMeasureClick_) {
            pendingMeasureClick_ = false;
            const QPoint position = event->position().toPoint();
            viewport_.measurementController_.setHoverPosition(
                {position.x(), position.y()});
            static_cast<void>(
                state_.queueMeasureCommit({position.x(), position.y()}));
            viewport_.requestRender();
            event->accept();
            return true;
        }
    }
    const bool releasesOrbit =
        event->button() == Qt::LeftButton && dragMode_ == DragMode::Orbit;
    const bool releasesPan =
        dragMode_ == DragMode::Pan &&
        (event->button() == Qt::RightButton ||
         (viewport_.mapView_ && event->button() == Qt::LeftButton));
    if (releasesOrbit || releasesPan) {
        dragMode_ = DragMode::None;
        viewport_.requestRender();
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::wheelEvent(QWheelEvent *event)
{
    double wheelUnits = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (wheelUnits == 0.0) {
        wheelUnits = static_cast<double>(event->pixelDelta().y()) / 120.0;
    }
    const QPoint position = event->position().toPoint();
    state_.queueWheel({position.x(), position.y()}, wheelUnits);
    viewport_.requestRender();
    event->accept();
    return true;
}

namespace {

std::optional<MovementKey> movementKey(const int key, const bool mapView)
{
    switch (key) {
    case Qt::Key_W:
        return MovementKey::Forward;
    case Qt::Key_S:
        return MovementKey::Backward;
    case Qt::Key_A:
        return MovementKey::Left;
    case Qt::Key_D:
        return MovementKey::Right;
    case Qt::Key_Q:
        return MovementKey::Down;
    case Qt::Key_E:
        if (!mapView) {
            return MovementKey::Up;
        }
        return std::nullopt;
    case Qt::Key_Z:
        if (mapView) {
            return MovementKey::Up;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace

bool ViewportInputController::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return true;
    }
    if (const auto key = movementKey(event->key(), viewport_.mapView_)) {
        state_.press(*key);
        viewport_.navigationTimer_.restart();
        viewport_.requestRender();
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Shift) {
        state_.setFast(true);
        if (state_.hasMovement()) {
            viewport_.requestRender();
        }
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Alt) {
        state_.setFine(true);
        if (state_.hasMovement()) {
            viewport_.requestRender();
        }
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_F) {
        if (viewport_.mapView_) {
            camera_.frameTopDown();
        } else {
            camera_.frameScene();
        }
        viewport_.requestRender();
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Escape &&
        viewport_.activeTool_ == ViewportTool::Measure) {
        state_.cancelPicks();
        viewport_.clearMeasurement();
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return true;
    }
    if (const auto key = movementKey(event->key(), viewport_.mapView_)) {
        state_.release(*key);
        viewport_.requestRender();
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Shift) {
        state_.setFast(false);
        if (state_.hasMovement()) {
            viewport_.requestRender();
        }
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Alt) {
        state_.setFine(false);
        if (state_.hasMovement()) {
            viewport_.requestRender();
        }
        event->accept();
        return true;
    }
    return false;
}

bool ViewportInputController::focusOutEvent(QFocusEvent *)
{
    state_.clearMovement();
    dragMode_ = DragMode::None;
    pendingMeasureClick_ = false;
    viewport_.measurementController_.clearHover();
    viewport_.updateMeasurementOverlay();
    viewport_.requestRender();
    return false;
}

} // namespace pci
