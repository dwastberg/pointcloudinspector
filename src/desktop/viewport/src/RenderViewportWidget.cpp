#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/viewport/RenderViewportWidget_p.h>
#include <pci/desktop/viewport/ViewportInputController.h>
#include <pci/rendering/FramePickGuard.h>

#include <pci/adapters/platform/ProcessMemory.h>
#include <pci/desktop/viewport/BackendPolicy.h>
#include <pci/desktop/viewport/RenderMetricsProjection.h>
#include <pci/foundation/CheckedArithmetic.h>
#include <pci/raster/RasterLayerDisplay.h>
#include <pci/rendering/planning/FrustumCuller.h>
#include <pci/rendering/planning/PointSizePolicy.h>
#include <pci/rendering/rhi/PointColorMapAtlas.h>

#include <QCoreApplication>
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
#include <QDebug>
#endif
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QVector3D>
#include <QWheelEvent>
#include <QWidget>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pci {
namespace {

void saturatingAccumulate(std::uint64_t &destination,
                          const std::uint64_t value) noexcept
{
    destination = saturatingAdd(destination, value);
}

std::chrono::nanoseconds
elapsedSince(const std::chrono::steady_clock::time_point start) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
}

#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
class DebugMarkScope {
public:
    DebugMarkScope(QRhiCommandBuffer *commandBuffer, const QByteArray &name)
        : commandBuffer_(commandBuffer)
    {
        commandBuffer_->debugMarkBegin(name);
    }

    ~DebugMarkScope()
    {
        commandBuffer_->debugMarkEnd();
    }

    DebugMarkScope(const DebugMarkScope &) = delete;
    DebugMarkScope &operator=(const DebugMarkScope &) = delete;

private:
    QRhiCommandBuffer *commandBuffer_;
};
#endif

} // namespace

namespace {

std::optional<QPointF> projectMeasurementPoint(const NavigationCamera &camera,
                                               const QSize size,
                                               const Vec3d point)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return std::nullopt;
    }
    const Vec3d relative = point - camera.position();
    const double depth = dot(relative, camera.forward());
    const auto clip = camera.clipPlanes();
    if (depth < clip.nearPlane || depth > clip.farPlane) {
        return std::nullopt;
    }
    const double aspect =
        static_cast<double>(size.width()) / static_cast<double>(size.height());
    if (!std::isfinite(aspect) || aspect <= 0.0) {
        return std::nullopt;
    }
    const double horizontal = dot(relative, camera.right());
    const double vertical = dot(relative, camera.up());
    double halfHeight = 0.0;
    if (camera.isOrthographic()) {
        halfHeight = camera.orthographicScale() * 0.5;
    } else {
        halfHeight =
            depth * std::tan(NavigationCamera::verticalFieldOfViewDegrees *
                             std::numbers::pi / 360.0);
    }
    if (!std::isfinite(halfHeight) || halfHeight <= 0.0) {
        return std::nullopt;
    }
    const double xNdc = horizontal / (halfHeight * aspect);
    const double yNdc = vertical / halfHeight;
    return QPointF((xNdc + 1.0) * static_cast<double>(size.width()) * 0.5,
                   (1.0 - yNdc) * static_cast<double>(size.height()) * 0.5);
}

QString measurementNumber(const double value)
{
    const double magnitude = std::abs(value);
    const char format =
        magnitude != 0.0 && (magnitude < 0.001 || magnitude >= 1'000'000.0)
            ? 'g'
            : 'f';
    return QString::number(value, format, format == 'g' ? 8 : 4);
}

} // namespace

class MeasurementOverlay final : public QWidget {
public:
    explicit MeasurementOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    void setState(const ViewportTool tool,
                  const NavigationCamera &camera,
                  const QPoint cursor,
                  const std::optional<Vec3d> &hover,
                  const std::optional<Vec3d> &anchor,
                  const std::optional<DistanceMeasurement> &measurement)
    {
        tool_ = tool;
        camera_ = camera;
        cursor_ = cursor;
        hover_ = hover;
        anchor_ = anchor;
        measurement_ = measurement;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (tool_ != ViewportTool::Measure) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent(QStringLiteral("#74a8ff"));
        const QColor halo(0, 0, 0, 185);
        const auto marker = [&](const QPointF point, const bool preview) {
            painter.setPen(QPen(halo, 5.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(
                point, preview ? 6.0 : 5.0, preview ? 6.0 : 5.0);
            painter.setPen(QPen(accent, 2.0));
            painter.drawEllipse(
                point, preview ? 6.0 : 5.0, preview ? 6.0 : 5.0);
        };
        const auto drawLine = [&](const QPointF first, const QPointF second) {
            painter.setPen(QPen(halo, 5.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(first, second);
            painter.setPen(QPen(accent, 2.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(first, second);
        };

        if (measurement_) {
            const auto start =
                projectMeasurementPoint(camera_, size(), measurement_->start);
            const auto end =
                projectMeasurementPoint(camera_, size(), measurement_->end);
            if (start && end) {
                drawLine(*start, *end);
                marker(*start, false);
                marker(*end, false);
                const QPointF midpoint = (*start + *end) * 0.5;
                const QString label =
                    QStringLiteral("3D %1 units  ·  Horizontal %2 units  ·  "
                                   "Vertical Δ %3 units")
                        .arg(
                            measurementNumber(measurement_->distance3d),
                            measurementNumber(measurement_->horizontalDistance),
                            measurementNumber(
                                std::abs(measurement_->verticalDelta)));
                const QFontMetrics metrics(font());
                const QSize labelSize =
                    metrics.size(Qt::TextSingleLine, label) + QSize(16, 10);
                const QPoint topLeft(
                    std::clamp(static_cast<int>(std::lround(midpoint.x())) -
                                   labelSize.width() / 2,
                               4,
                               std::max(4, width() - labelSize.width() - 4)),
                    std::clamp(static_cast<int>(std::lround(midpoint.y())) -
                                   labelSize.height() - 10,
                               4,
                               std::max(4, height() - labelSize.height() - 4)));
                const QRect labelRect(topLeft, labelSize);
                painter.setPen(QPen(QColor(QStringLiteral("#343b44"))));
                painter.setBrush(QColor(17, 20, 25, 230));
                painter.drawRoundedRect(labelRect, 4.0, 4.0);
                painter.setPen(QColor(QStringLiteral("#eef1f5")));
                painter.drawText(labelRect, Qt::AlignCenter, label);
            }
        } else if (anchor_) {
            if (const auto point =
                    projectMeasurementPoint(camera_, size(), *anchor_)) {
                marker(*point, false);
            }
        }

        if (hover_) {
            if (const auto point =
                    projectMeasurementPoint(camera_, size(), *hover_)) {
                if (QLineF(cursor_, *point).length() > 2.0) {
                    painter.setPen(QPen(accent, 1.0, Qt::DashLine));
                    painter.drawLine(cursor_, *point);
                }
                marker(*point, true);
            }
        }

        painter.setPen(QColor(235, 239, 245, 190));
        const QString hint =
            anchor_
                ? QStringLiteral("Click a second point to measure · Esc clears")
                : QStringLiteral("Click a point to start measuring · Drag to "
                                 "orbit · Esc clears");
        painter.drawText(QRect(16, height() - 34, width() - 32, 20),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         hint);
    }

private:
    ViewportTool tool_ = ViewportTool::Navigate;
    NavigationCamera camera_;
    QPoint cursor_;
    std::optional<Vec3d> hover_;
    std::optional<Vec3d> anchor_;
    std::optional<DistanceMeasurement> measurement_;
};

RenderViewportWidget::RenderViewportWidget(
    const bool smokeTest,
    const std::uint64_t gpuByteBudget,
    const GraphicsApi graphicsApi,
    const bool enableGpuValidation,
    PointColorMapCatalogSnapshotPtr colorMaps)
    : smokeTest_(smokeTest)
    , requestedGraphicsApi_(graphicsApi)
    , gpuValidationEnabled_(enableGpuValidation)
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    , profileRendering_(qEnvironmentVariableIsSet("PCI_PROFILE_RENDERING"))
    , probeResidency_(qEnvironmentVariableIsSet("PCI_PROBE_RESIDENCY"))
#endif
    , pointBudget_(1)
    , colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
    , renderer_(gpuByteBudget, colorMaps_)
{
    if (!graphicsApiSupportedOnPlatform(graphicsApi)) {
        throw std::invalid_argument("graphics API '" +
                                    std::string(graphicsApiName(graphicsApi)) +
                                    "' is not supported on this platform");
    }
    if (const auto explicitApi = explicitBackendApi(graphicsApi)) {
        setApi(*explicitApi);
    }
    setDebugLayerEnabled(enableGpuValidation);
    setFocusPolicy(Qt::StrongFocus);
    inputController_ = std::make_unique<ViewportInputController>(*this);
    completionExecutor_ = makeQtCompletionExecutor(this);
    sceneSnapshotCache_.setCallbacks(
        [this] {
            queueSceneInvalidation();
        },
        [executor = completionExecutor_](std::function<void()> callback) {
            return executor->post(
                CompletionExecutor::Completion{std::move(callback)});
        });
    frameExecutor_.rasters().setWakeCallback([executor = completionExecutor_,
                                              this] {
        static_cast<void>(executor->post(CompletionExecutor::Completion{[this] {
            queueSceneInvalidation();
        }}));
    });
    navigationTimer_.start();
    measurementOverlay_ = new MeasurementOverlay(this);
    measurementOverlay_->setGeometry(rect());
    measurementOverlay_->show();
    measurementHoverTimer_ = new QTimer(this);
    measurementHoverTimer_->setSingleShot(true);
    connect(measurementHoverTimer_, &QTimer::timeout, this, [this] {
        if (const auto position = measurementController_.takeScheduledHover()) {
            queueMeasurementHover(*position);
        }
    });
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        fail(QStringLiteral(
                 "QRhiWidget could not render (requested '%1', selected %2). "
                 "Retry with --graphics-api auto or another supported backend.")
                 .arg(QString::fromUtf8(
                          graphicsApiName(requestedGraphicsApi_).data(),
                          static_cast<qsizetype>(
                              graphicsApiName(requestedGraphicsApi_).size())),
                      backendApiName(api())));
    });
}

RenderViewportWidget::~RenderViewportWidget()
{
    *pickDelivery_ = false;
    completionExecutor_->invalidate();
    // Workers may otherwise enqueue a wake while the QObject and its retained
    // scene sources are being dismantled. shutdown() is idempotent and the
    // streamer's member destructor repeats it defensively.
    frameExecutor_.rasters().shutdown();
    sceneSnapshotCache_.clear();
}

QWidget *RenderViewportWidget::widget() noexcept
{
    return this;
}

QString RenderViewportWidget::backendName() const
{
    return backendApiName(api());
}

std::uint64_t RenderViewportWidget::totalPointCount() const noexcept
{
    return pointCount_;
}

void RenderViewportWidget::setDocument(
    SceneDocumentSnapshotPtr document,
    SceneRuntimeSnapshotPtr runtime,
    const RuntimeBudgetSnapshot runtimeBudget,
    const SessionGeneration sessionGeneration,
    const bool frameVisibleLayers)
{
    if (!document) {
        throw std::invalid_argument("document must not be null");
    }
    const bool replacingDocument =
        static_cast<bool>(sceneSnapshotCache_.document());
    sessionGeneration_ = sessionGeneration;
    if (!runtime) {
        throw std::invalid_argument("runtime snapshot must not be null");
    }
    if (!runtimeBudget.valid()) {
        throw std::invalid_argument("runtime budget snapshot must be valid");
    }
    runtimeBudget_ = runtimeBudget;
    runtimeSnapshot_ = std::move(runtime);
    sceneSnapshotCache_.setDocument(
        std::move(document), runtimeSnapshot_, true);
    pendingFirstFrameLayerIds_.clear();
    pendingDisplayReadyLayerIds_.clear();
    publishedResidentPointCounts_.clear();
    hierarchyLayerErrors_.clear();
    frameExecutor_.points().clear();
    frameExecutor_.clear();
    drawSignature_.clear();
    ++selectionGeneration_;
    frameExecutor_.telemetry().setPicking({});
    if (replacingDocument) {
        renderer_.uploads.retainLayers({});
    }
    const SceneDocumentSnapshotPtr &currentDocument =
        sceneSnapshotCache_.document();
    pointCount_ = currentDocument->visibleExpectedPointCount;
    frameExecutor_.telemetry().setSourceTotals(pointCount_, 0);
    pointBudget_ = AdaptivePointBudget(std::max<std::uint64_t>(pointCount_, 1));
    inputController_->state().cancelPicks();
    rawPickCompletion_ = {};
    if (replacingDocument) {
        clearMeasurement();
    }

    if (frameVisibleLayers) {
        this->frameVisibleLayers();
    }
    requestRender();
}

void RenderViewportWidget::updateDocument(
    SceneDocumentSnapshotPtr document,
    SceneRuntimeSnapshotPtr runtime,
    const RuntimeBudgetSnapshot runtimeBudget,
    const SessionGeneration sessionGeneration)
{
    if (!document) {
        throw std::invalid_argument("document must not be null");
    }
    sessionGeneration_ = sessionGeneration;
    if (!runtime) {
        throw std::invalid_argument("runtime snapshot must not be null");
    }
    if (!runtimeBudget.valid()) {
        throw std::invalid_argument("runtime budget snapshot must be valid");
    }
    runtimeBudget_ = runtimeBudget;
    runtimeSnapshot_ = std::move(runtime);
    sceneSnapshotCache_.setDocument(
        std::move(document), runtimeSnapshot_, false);
    pointCount_ = sceneSnapshotCache_.document()->visibleExpectedPointCount;
    requestRender();
}

void RenderViewportWidget::requestRender()
{
    updateMeasurementOverlay();
    update();
}

void RenderViewportWidget::frameVisibleLayers()
{
    // Fit Scene is a deliberate, user-invoked framing command. Use a tighter
    // margin than the historical camera default so the visible bounds occupy
    // the viewport more effectively.
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    frameBounds(document ? document->visibleBounds : std::nullopt, 1.15);
    // Framing changes the camera without otherwise invalidating the scene.
    // Explicitly schedule a frame so Fit Scene repaints immediately.
    requestRender();
}

void RenderViewportWidget::frameVisibleLayersTopDown()
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    frameBounds(document ? document->visibleBounds : std::nullopt, 1.15);
    if (!mapView_) {
        inputController_->camera().frameTopDown(1.15);
    }
    requestRender();
}

bool RenderViewportWidget::isOrthographic() const noexcept
{
    return orthographic_;
}

void RenderViewportWidget::setOrthographic(const bool enabled)
{
    if (mapView_ && !enabled) {
        return;
    }
    if (orthographic_ == enabled) {
        return;
    }
    orthographic_ = enabled;
    inputController_->camera().setOrthographic(enabled);
    requestRender();
}

bool RenderViewportWidget::isMapView() const noexcept
{
    return mapView_;
}

void RenderViewportWidget::setMapView(const bool enabled)
{
    if (mapView_ == enabled) {
        return;
    }

    mapView_ = enabled;
    inputController_->state().clearMovement();
    inputController_->resetGesture();
    if (!enabled) {
        orthographic_ = orthographicBeforeMapView_;
        inputController_->camera().setOrthographic(orthographic_);
        requestRender();
        return;
    }

    orthographicBeforeMapView_ = orthographic_;
    orthographic_ = true;
    inputController_->camera().setOrthographic(true);
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    frameBounds(document ? document->visibleBounds : std::nullopt, 1.15);
    requestRender();
}

void RenderViewportWidget::frameBounds(const std::optional<Bounds3d> &bounds,
                                       const double distanceMultiplier)
{
    if (bounds && bounds->valid() && bounds->maximumExtent() > 0.0) {
        const auto center = bounds->center();
        inputController_->camera().setScene({center[0], center[1], center[2]},
                                            bounds->maximumExtent());
    } else {
        inputController_->camera().setScene({0.0, 0.0, 0.0}, 2.0);
    }
    if (mapView_) {
        inputController_->camera().frameTopDown(distanceMultiplier);
    } else {
        inputController_->camera().frameScene(distanceMultiplier);
    }
}

void RenderViewportWidget::frameLayer(const PointCloudLayerId layerId)
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    if (!document) {
        return;
    }
    const std::optional<Bounds3d> bounds = document->layerBounds(layerId);
    if (!bounds) {
        return;
    }
    frameBounds(bounds);
    requestRender();
}

bool RenderViewportWidget::eyeDomeLightingEnabled() const noexcept
{
    return viewportSettings_.depthEnhancement.enabled;
}

void RenderViewportWidget::setEyeDomeLightingEnabled(const bool enabled)
{
    if (viewportSettings_.depthEnhancement.enabled == enabled) {
        return;
    }
    viewportSettings_.depthEnhancement.enabled = enabled;
    requestRender();
}

ViewportSettings RenderViewportWidget::viewportSettings() const noexcept
{
    return viewportSettings_;
}

void RenderViewportWidget::setViewportSettings(const ViewportSettings &settings)
{
    const auto finiteColor = [](const float value, const float fallback) {
        return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : fallback;
    };
    ViewportSettings bounded = settings;
    bounded.backgroundColor.red =
        finiteColor(bounded.backgroundColor.red, defaultBackgroundRed);
    bounded.backgroundColor.green =
        finiteColor(bounded.backgroundColor.green, defaultBackgroundGreen);
    bounded.backgroundColor.blue =
        finiteColor(bounded.backgroundColor.blue, defaultBackgroundBlue);
    bounded.depthEnhancement.radius =
        std::isfinite(bounded.depthEnhancement.radius)
            ? std::clamp(bounded.depthEnhancement.radius,
                         minimumDepthEnhancementRadius,
                         maximumDepthEnhancementRadius)
            : defaultDepthEnhancementRadius;
    bounded.depthEnhancement.strength =
        std::isfinite(bounded.depthEnhancement.strength)
            ? std::clamp(bounded.depthEnhancement.strength,
                         minimumDepthEnhancementStrength,
                         maximumDepthEnhancementStrength)
            : defaultDepthEnhancementStrength;
    if (viewportSettings_ == bounded) {
        return;
    }
    viewportSettings_ = bounded;
    requestRender();
}

std::uint64_t RenderViewportWidget::gpuByteBudget() const noexcept
{
    return pendingPointGpuBudget_.value_or(
        renderer_.uploads.residencyByteBudget());
}

void RenderViewportWidget::setGpuByteBudget(const std::uint64_t byteBudget)
{
    if (gpuByteBudget() == byteBudget) {
        return;
    }
    if (byteBudget == 0) {
        throw std::invalid_argument("GPU residency budget must be positive");
    }
    pendingPointGpuBudget_ = byteBudget;
    queueSceneInvalidation();
}

void RenderViewportWidget::setRasterByteBudgets(
    const std::uint64_t cpuByteBudget, const std::uint64_t gpuByteBudget)
{
    pendingRasterBudgets_ = std::pair{cpuByteBudget, gpuByteBudget};
    queueSceneInvalidation();
}

int RenderViewportWidget::pointSizePixels() const noexcept
{
    return pointSizePixels_;
}

ViewportTool RenderViewportWidget::activeTool() const noexcept
{
    return activeTool_;
}

void RenderViewportWidget::setActiveTool(const ViewportTool tool)
{
    if (activeTool_ == tool) {
        return;
    }
    activeTool_ = tool;
    inputController_->resetGesture();
    inputController_->state().cancelPicks();
    if (measurementHoverTimer_) {
        measurementHoverTimer_->stop();
    }
    clearMeasurement();
    if (activeTool_ == ViewportTool::Measure) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
    updateMeasurementOverlay();
    requestRender();
}

void RenderViewportWidget::setPointSizePixels(const int pointSize)
{
    const int boundedPointSize =
        std::clamp(pointSize, minimumPointSizePixels, maximumPointSizePixels);
    if (pointSizePixels_ == boundedPointSize) {
        return;
    }
    pointSizePixels_ = boundedPointSize;
    requestRender();
}

void RenderViewportWidget::setMetricsCallback(MetricsCallback callback)
{
    metricsCallback_ = std::move(callback);
}

void RenderViewportWidget::setContinuousMetricsEnabled(const bool enabled)
{
    if (continuousMetricsEnabled_ == enabled) {
        return;
    }
    continuousMetricsEnabled_ = enabled;
    frameExecutor_.telemetry().makeNextSnapshotDue(
        RenderTelemetryAccumulator::Clock::now());
    if (enabled) {
        requestRender();
    }
}

bool RenderViewportWidget::startQualificationCameraPath()
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    if (!document || !document->visibleBounds ||
        !document->visibleBounds->valid()) {
        return false;
    }
    qualificationCameraPath_.emplace(*document->visibleBounds);
    mapView_ = false;
    orthographic_ = false;
    inputController_->camera().setOrthographic(false);
    applyQualificationCameraFrame();
    requestRender();
    return true;
}

void RenderViewportWidget::applyQualificationCameraFrame()
{
    if (!qualificationCameraPath_) {
        return;
    }
    const QualificationCameraFrame frame = qualificationCameraPath_->current();
    inputController_->camera().setView(frame.pose.position, frame.pose.pivot);
}

void RenderViewportWidget::setFailureCallback(FailureCallback callback)
{
    failureCallback_ = std::move(callback);
}

void RenderViewportWidget::setLoadProgressCallback(
    LoadProgressCallback callback)
{
    loadProgressCallback_ = std::move(callback);
}

VectorOverlayCapability
RenderViewportWidget::vectorOverlayCapability() const noexcept
{
    return vectorOverlayCapability_;
}

void RenderViewportWidget::setVectorOverlayCapabilityCallback(
    VectorOverlayCapabilityCallback callback)
{
    vectorOverlayCapabilityCallback_ = std::move(callback);
    if (vectorOverlayCapabilityCallback_) {
        vectorOverlayCapabilityCallback_(vectorOverlayCapability_, {});
    }
}

RasterSurfaceCapability
RenderViewportWidget::rasterSurfaceCapability() const noexcept
{
    return rasterSurfaceCapability_;
}

void RenderViewportWidget::setRasterSurfaceCapabilityCallback(
    RasterSurfaceCapabilityCallback callback)
{
    rasterSurfaceCapabilityCallback_ = std::move(callback);
    if (rasterSurfaceCapabilityCallback_) {
        rasterSurfaceCapabilityCallback_(rasterSurfaceCapability_,
                                         rasterSurfaceCapabilityReason_);
    }
}

void RenderViewportWidget::initialize(QRhiCommandBuffer *commandBuffer)
{
    Q_UNUSED(commandBuffer);
    if (failed_) {
        return;
    }

    try {
        if (rhi()->backend() != rhiImplementationFor(api())) {
            throw std::runtime_error(
                "QRhi did not select the requested backend: " +
                backendApiName(api()).toStdString());
        }
        if (!rhi()->isFeatureSupported(QRhi::IntAttributes)) {
            throw std::runtime_error(
                "GPU device does not support integer vertex attributes");
        }
        if (!rhi()->isFeatureSupported(QRhi::VertexShaderPointSize)) {
            throw std::runtime_error(
                "GPU device does not support vertex shader point size");
        }

        if (resourceRhi_ != rhi()) {
            releaseResources();
            resourceRhi_ = rhi();
            const QRhiDriverInfo driver = rhi()->driverInfo();
            deviceName_ = QString::fromUtf8(driver.deviceName);
            if (deviceName_.isEmpty()) {
                deviceName_ = QStringLiteral("GPU device");
            }
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
            qInfo().noquote()
                << QStringLiteral("Renderer backend: requested=%1 selected=%2 "
                                  "validation=%3 device=%4")
                       .arg(QString::fromUtf8(
                                graphicsApiName(requestedGraphicsApi_).data(),
                                static_cast<qsizetype>(
                                    graphicsApiName(requestedGraphicsApi_)
                                        .size())),
                            backendApiName(api()),
                            gpuValidationEnabled_ ? QStringLiteral("on")
                                                  : QStringLiteral("off"),
                            deviceName_);
#endif
            previousCpuFrame_ = std::chrono::steady_clock::now();
            frameExecutor_.telemetry().makeNextSnapshotDue(previousCpuFrame_);
            frameTimingContinuous_ = false;
        }
        static_cast<void>(ensureRenderResources());
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
    }
}

bool RenderViewportWidget::ensureRenderResources()
{
    const VectorOverlayCapability capability =
        rhi()->isFeatureSupported(QRhi::Instancing)
            ? VectorOverlayCapability::Supported
            : VectorOverlayCapability::Unsupported;
    if (capability != vectorOverlayCapability_) {
        vectorOverlayCapability_ = capability;
        if (vectorOverlayCapabilityCallback_) {
            vectorOverlayCapabilityCallback_(
                capability,
                capability == VectorOverlayCapability::Unsupported
                    ? QStringLiteral(
                          "This graphics backend does not support instancing.")
                    : QString{});
        }
    }
    const QSize outputSize = renderTarget()->pixelSize();
    if (eyeDomeLightingActive_ &&
        !renderer_.edl.matchesPointTarget(rhi(), outputSize)) {
        // The point pipeline references the EDL target's render-pass
        // descriptor. Destroy that pipeline before replacing the target.
        renderer_.points.releaseResources();
        renderer_.vectors.releaseResources();
        renderer_.rasters.releaseResources();
        renderer_.edl.releaseResources();
        eyeDomeLightingActive_ = false;
    }

    if (viewportSettings_.depthEnhancement.enabled) {
        eyeDomeLightingActive_ = renderer_.edl.ensureResources(
            rhi(), outputSize, renderTarget()->renderPassDescriptor());
    } else {
        // Switch the point pipeline away from the EDL descriptor before
        // releasing the target that owns it.
        renderer_.points.ensureResources(
            rhi(), renderTarget()->renderPassDescriptor());
        renderer_.edl.releaseResources();
        eyeDomeLightingActive_ = false;
    }

    QRhiRenderPassDescriptor *pointRenderPass =
        renderTarget()->renderPassDescriptor();
    if (eyeDomeLightingActive_) {
        pointRenderPass = renderer_.edl.pointRenderPassDescriptor();
    }
    renderer_.points.ensureResources(rhi(), pointRenderPass);
    // Overlays always record into the widget target: when EDL is active this
    // is the post-composite pass, where republished point depth remains usable.
    renderer_.rasters.ensureResources(rhi(),
                                      renderTarget()->renderPassDescriptor());
    const RasterSurfaceCapability rasterCapability =
        renderer_.rasters.surfaceSupported()
            ? RasterSurfaceCapability::Supported
            : RasterSurfaceCapability::Unsupported;
    const QString rasterReason =
        QString::fromStdString(renderer_.rasters.surfaceCapabilityReason());
    if (rasterCapability != rasterSurfaceCapability_ ||
        rasterReason != rasterSurfaceCapabilityReason_) {
        rasterSurfaceCapability_ = rasterCapability;
        rasterSurfaceCapabilityReason_ = rasterReason;
        if (rasterSurfaceCapabilityCallback_) {
            rasterSurfaceCapabilityCallback_(rasterSurfaceCapability_,
                                             rasterSurfaceCapabilityReason_);
        }
    }
    renderer_.vectors.ensureResources(rhi(),
                                      renderTarget()->renderPassDescriptor());
    renderer_.picker.ensureResources(rhi(), renderer_.points.shaderBindings());
    return eyeDomeLightingActive_;
}

std::chrono::duration<double, std::milli>
RenderViewportWidget::frameDuration(QRhiCommandBuffer *commandBuffer)
{
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration<double, std::milli>(now - previousCpuFrame_);
    previousCpuFrame_ = now;
    if (!frameTimingContinuous_) {
        return {};
    }

    const double gpuSeconds = commandBuffer->lastCompletedGpuTime();
    if (gpuSeconds > 0.0) {
        timingSource_ = QStringLiteral("GPU");
        return std::chrono::duration<double, std::milli>(gpuSeconds * 1000.0);
    }

    timingSource_ = QStringLiteral("CPU");
    return elapsed;
}

// One frame of raster streaming: plan the visible tiles, reconcile the read
// queue, admit finished reads, upload what the frame's budget allows, and
// build the draws for whatever is resident.
RenderViewportWidget::RasterFrameCapture
RenderViewportWidget::captureRasterFrame()
{
    RasterFrameCapture capture;
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    rasterDrawnTiles_ = 0;
    rasterSurfaceDrawnTiles_ = 0;

    // An empty document is deliberately not an early return: releasing removed
    // sources and pruning per-layer state is exactly what a removed layer
    // needs, and skipping it strands that layer's in-flight reads in a
    // completion queue nothing ever drains.
    auto &layers = capture.layers;
    if (document) {
        const RasterLayerSnapshotView view = document->rasterLayers();
        layers.assign(view.begin(), view.end());
    }
    std::vector<RasterTileSourcePtr> sources(layers.size());
    if (runtimeSnapshot_) {
        for (std::size_t index = 0; index < layers.size(); ++index) {
            sources[index] =
                runtimeSnapshot_->raster(layers[index].descriptor.sourceId,
                                         layers[index].bindingGeneration);
        }
    }

    const FrameCamera frame = currentFrameCamera();
    auto &rasterInputs = capture.inputs;
    rasterInputs.reserve(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const RasterLayerSnapshot &layer = layers[index];
        const RasterTileSourcePtr &source = sources[index];
        std::shared_ptr<const RasterDecodeParameters> decode;
        if (source) {
            RasterDecodeState &decodeState = rasterDecodeStates_[layer.id];
            if (!decodeState.parameters ||
                decodeState.generation != layer.renderGeneration) {
                decodeState.generation = layer.renderGeneration;
                decodeState.parameters = resolveRasterDecodeParameters(
                    layer.descriptor.metadata, layer.style, *colorMaps_);
            }
            decode = decodeState.parameters;
        }
        rasterInputs.push_back(RasterFrameLayerInput{
            .layerId = layer.id,
            .sourceId = layer.descriptor.sourceId,
            .bindingGeneration = layer.bindingGeneration,
            .renderGeneration = layer.renderGeneration,
            .visible = layer.visible,
            .layer = RasterLodLayerView{layer.descriptor.metadata,
                                        layer.style,
                                        layer.elevationStatus,
                                        layer.exactElevationRange},
            .sourceAvailable = bool(source),
            .decode = std::move(decode),
        });
    }

    capture.input = RasterFrameInput{
        .layers = rasterInputs,
        .camera = frame,
        .colorTileCapacity = renderer_.rasters.tileCapacity(
            RasterTilePayloadProfile::ColorOnly,
            pendingRasterBudgets_ ? std::optional{pendingRasterBudgets_->second}
                                  : std::nullopt),
        .elevationTileCapacity = renderer_.rasters.tileCapacity(
            RasterTilePayloadProfile::RenderElevation,
            pendingRasterBudgets_ ? std::optional{pendingRasterBudgets_->second}
                                  : std::nullopt),
        .surfaceSupported = renderer_.rasters.surfaceSupported(),
        .cpuResident =
            [this](const RasterCacheKey &key) {
                return frameExecutor_.rasters().cpuResident(key);
            },
        .gpuResident =
            [this](const RasterCacheKey &key) {
                return renderer_.rasters.gpuResident(key);
            },
        .unavailable =
            [this](const RasterCacheKey &key) {
                return frameExecutor_.rasters().failed(key);
            },
    };
    return capture;
}

void RenderViewportWidget::render(QRhiCommandBuffer *commandBuffer)
{
    if (failed_) {
        return;
    }

    try {
        static_cast<void>(ensureRenderResources());
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }
    if (!renderer_.points.ready() || !renderer_.vectors.ready() ||
        !renderer_.rasters.ready()) {
        return;
    }

    sceneInvalidationPending_ = false;
    updateKeyboardNavigation();
    const auto duration = frameDuration(commandBuffer);
    if (frameExecutor_.telemetry().frameCount() > 0 && duration.count() > 0.0) {
        const bool delayedGpuWork =
            timingSource_ == QStringLiteral("GPU") &&
            frameExecutor_.telemetry().specialFrameCooldownActive();
        pointBudget_.update({
            .frameTime = duration,
            .gpuTiming = timingSource_ == QStringLiteral("GPU"),
            .includedUploads =
                frameExecutor_.telemetry().previousFrameIncludedUploads() ||
                delayedGpuWork,
            .includedPick =
                frameExecutor_.telemetry().previousFrameIncludedPick() ||
                delayedGpuWork,
            .submittedPoints = frameExecutor_.telemetry().submittedPoints(),
        });
    }
    frameExecutor_.telemetry().advanceSpecialFrameCooldown();

    std::vector<BlockDraw> draws;
    std::vector<VectorLayerDraw> vectorDraws;
    std::vector<RasterLayerDraw> rasterDraws;
    std::uint64_t selectedPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    std::uint64_t rootOnlyLayerCount = 0;
    std::uint64_t requestedPoints = 0;
    std::uint64_t vectorLayersDrawn = 0;
    std::uint64_t vectorDrawCalls = 0;
    bool framePlanReused = false;
    std::size_t uploaded = 0;
    bool uploadsNeedAnotherFrame = false;
    bool planNeedsAnotherFrame = false;
    bool rasterNeedsAnotherFrame = false;
    const bool frameStartedWithPick =
        inputController_->state().pendingPick().has_value() ||
        renderer_.picker.inFlight();
    std::chrono::nanoseconds snapshotTime{};
    std::chrono::nanoseconds selectionTime{};
    std::chrono::nanoseconds uploadTime{};
    std::chrono::nanoseconds commandTime{};
    try {
        const auto snapshotStart = std::chrono::steady_clock::now();
        const SceneDocumentSnapshotPtr &document =
            sceneSnapshotCache_.document();
        if (document) {
            std::vector<LayerFrameState> layers;
            RasterFrameCapture rasterCapture;
            auto selectionStart = std::chrono::steady_clock::now();
            bool allFlat = false;
            frameExecutor_.run({
                .context =
                    [this] {
                        return FrameContext{sessionGeneration_,
                                            sceneSnapshotCache_.document(),
                                            runtimeSnapshot_};
                    },
                .capture = [&]() -> FramePlanningInput {
                    std::vector<PointCloudLayerId> retainedLayerIds;
                    const Bounds3d documentColorBounds =
                        document->bounds.value_or(Bounds3d{});
                    const SceneSnapshotCache::RefreshResult snapshotRefresh =
                        sceneSnapshotCache_.refresh();
                    for (const PointCloudLayerId layerId :
                         snapshotRefresh.invalidatedRootPayloads) {
                        renderer_.uploads.invalidateNode(layerId,
                                                         rootPointCloudNode);
                    }
                    for (const PointCloudLayerId layerId :
                         snapshotRefresh.invalidatedColors) {
                        renderer_.uploads.invalidateLayer(layerId);
                    }
                    for (const SceneSnapshotLayer &sceneLayer :
                         document->layers) {
                        const auto *point =
                            std::get_if<PointCloudLayerSnapshotState>(
                                &sceneLayer.payload);
                        if (!point) {
                            continue;
                        }
                        retainedLayerIds.push_back(sceneLayer.id);
                        if (!sceneLayer.visible) {
                            continue;
                        }
                        const PointDatasetRuntimeSnapshot *cached =
                            sceneSnapshotCache_.snapshot(sceneLayer.id);
                        if (!cached) {
                            continue;
                        }
                        const PointDatasetRuntimePtr runtime =
                            runtimeSnapshot_->point(
                                point->descriptor.sourceId,
                                sceneLayer.bindingGeneration);
                        Bounds3d sourceBounds =
                            point->descriptor.metadata.sourceBounds;
                        if (!sourceBounds.valid()) {
                            sourceBounds = cached->bounds;
                        }
                        layers.push_back({
                            .layerId = sceneLayer.id,
                            .sourceId = point->descriptor.sourceId,
                            .bindingGeneration = sceneLayer.bindingGeneration,
                            .colorMode = point->colorMode,
                            .classificationFilter = point->classificationFilter,
                            .sourceBounds = sourceBounds,
                            .residency = runtime ? runtime->residencyView()
                                                 : PointResidencyViewPtr{},
                            .snapshot = cached,
                            .colorRange =
                                effectivePointColorRange(point->colorMode,
                                                         documentColorBounds,
                                                         cached->scalarRanges),
                        });
                    }
                    snapshotTime = elapsedSince(snapshotStart);

                    renderer_.uploads.beginFrame(
                        frameExecutor_.telemetry().nextFrameNumber());
                    renderer_.uploads.retainLayers(retainedLayerIds);
                    refreshDocumentState(layers);
                    const std::uint64_t currentFramePointBudget =
                        framePointBudget(layers);
                    requestedPoints = currentFramePointBudget;

                    selectionStart = std::chrono::steady_clock::now();
                    allFlat = std::ranges::all_of(
                        layers, [](const LayerFrameState &layer) {
                            return !layer.snapshot->hierarchical;
                        });
                    rasterCapture = captureRasterFrame();
                    return {
                        PointFrameInput{
                            .sessionGeneration = sessionGeneration_,
                            .documentGeneration = document->generation,
                            .documentRevision = document->revision,
                            .layers = layers,
                            .camera = currentFrameCamera(),
                            .cameraRevision =
                                inputController_->camera().revision(),
                            .framePointBudget = currentFramePointBudget,
                            .gpuByteBudget = pendingPointGpuBudget_.value_or(
                                renderer_.uploads.residencyByteBudget()),
                            .resident =
                                [this](const PointFrameBlockKey &key) {
                                    return renderer_.uploads.bufferFor({
                                               .layerId = key.layerId,
                                               .nodeId = key.nodeId,
                                               .nodeBlockIndex =
                                                   key.nodeBlockIndex,
                                               .sceneBlockId = key.sceneBlockId,
                                           }) != nullptr;
                                },
                        },
                        rasterCapture.input};
                },
                .protect =
                    [&](const PointFramePlan &point,
                        const RasterFramePlan &raster) {
                        currentGpuProtection_ = point.protectedGpuBlocks;
                        if (renderer_.picker.inFlight()) {
                            currentGpuProtection_.insert(
                                currentGpuProtection_.end(),
                                pickGpuProtection_.begin(),
                                pickGpuProtection_.end());
                        } else {
                            pickGpuProtection_.clear();
                        }

                        if (pendingPointGpuBudget_) {
                            renderer_.uploads.setResidencyByteBudget(
                                *pendingPointGpuBudget_, currentGpuProtection_);
                            pendingPointGpuBudget_.reset();
                        }
                        if (pendingRasterBudgets_) {
                            frameExecutor_.rasters().setCpuByteBudget(
                                pendingRasterBudgets_->first,
                                raster.protectedTiles);
                            renderer_.rasters.setGpuByteBudget(
                                pendingRasterBudgets_->second,
                                raster.protectedTiles);
                            pendingRasterBudgets_.reset();
                        }
                    },
                .submit =
                    [&](const PointFrameResult &pointFrame,
                        const RasterFramePlan &rasterPlan) {
                        selectionTime = elapsedSince(selectionStart);
                        const PointFramePlan *plan = pointFrame.plan.get();
                        framePlanReused = pointFrame.reused;
                        outOfFrustumLayerIds_ = plan->outOfFrustumLayerIds;
                        selectedPoints = plan->selectedPoints;
                        visibleBlocks = plan->visibleBlocks;
                        culledBlocks = plan->culledBlocks;
                        visibleLayerCount = plan->visibleLayerCount;
                        coveredLayerCount = plan->coveredLayerCount;
                        rootOnlyLayerCount = plan->rootOnlyLayerCount;
                        planNeedsAnotherFrame =
                            planNeedsAnotherFrame || plan->requiresContinuation;
                        std::unordered_map<PointCloudLayerId, std::string>
                            currentErrors;
                        currentErrors.reserve(plan->layerErrors.size());
                        for (const PointFrameLayerError &error :
                             plan->layerErrors) {
                            currentErrors.emplace(error.layerId, error.message);
                            const auto previous =
                                hierarchyLayerErrors_.find(error.layerId);
                            if (previous == hierarchyLayerErrors_.end() ||
                                previous->second != error.message) {
                                qWarning().noquote()
                                    << QStringLiteral("Point-cloud layer %1 "
                                                      "was dropped from the "
                                                      "frame: %2")
                                           .arg(error.layerId.value())
                                           .arg(QString::fromStdString(
                                               error.message));
                            }
                        }
                        hierarchyLayerErrors_ = std::move(currentErrors);

                        const auto uploadStart =
                            std::chrono::steady_clock::now();
                        {
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
                            const DebugMarkScope uploadMarker(
                                commandBuffer,
                                QByteArrayLiteral("Point uploads"));
#endif
                            uploaded = renderer_.uploads.uploadPending(
                                rhi(),
                                commandBuffer,
                                plan->uploads,
                                currentGpuProtection_);
                        }
                        uploadTime = elapsedSince(uploadStart);
                        const bool uploadsRemain = std::ranges::any_of(
                            plan->uploads, [this](const UploadBlock &upload) {
                                return !renderer_.uploads.bufferFor(upload.key);
                            });
                        // Continue only while the previous frame made progress.
                        // An upload that cannot fit the configured residency
                        // budget must not create a permanent busy loop.
                        // Hierarchical selection is evaluated before uploads,
                        // so even the final upload needs one follow-up frame to
                        // make the node drawable or commit a refinement.
                        uploadsNeedAnotherFrame =
                            uploaded > 0 && (uploadsRemain || !allFlat);
                        for (const LayerFrameState &layer : layers) {
                            const std::uint64_t resident =
                                renderer_.uploads.residentPointCount(
                                    layer.layerId);
                            const std::uint64_t previous =
                                publishedResidentPointCounts_[layer.layerId];
                            publishedResidentPointCounts_[layer.layerId] =
                                resident;
                            const std::uint64_t total =
                                layer.snapshot->hierarchical
                                    ? layer.snapshot->sourcePointCount
                                    : layer.snapshot->retainedFlatPointCount;
                            if (resident != previous && resident < total) {
                                publishLoadProgress({
                                    .layerId = layer.layerId,
                                    .bindingGeneration =
                                        layer.bindingGeneration,
                                    .stage = RenderLoadStage::Uploading,
                                    .completed = resident,
                                    .total = total,
                                });
                            }
                        }
                        const auto commandStart =
                            std::chrono::steady_clock::now();
                        draws = renderer_.pointDraws(rhi(),
                                                     currentFrameCamera(),
                                                     *plan,
                                                     pointSizePixels_);
                        vectorDraws = renderer_.vectorDraws(
                            rhi(), currentFrameCamera(), document);
                        vectorLayersDrawn = vectorDraws.size();
                        for (const VectorLayerDraw &draw : vectorDraws) {
                            if (!draw.layer.data) {
                                continue;
                            }
                            vectorDrawCalls = saturatingAdd(
                                vectorDrawCalls,
                                static_cast<std::uint64_t>(
                                    draw.layer.data->fillBatches.size()));
                            vectorDrawCalls = saturatingAdd(
                                vectorDrawCalls,
                                std::uint64_t{draw.layer.data->segments.empty()
                                                  ? 0U
                                                  : 1U});
                            vectorDrawCalls = saturatingAdd(
                                vectorDrawCalls,
                                std::uint64_t{draw.layer.data->markers.empty()
                                                  ? 0U
                                                  : 1U});
                        }
                        updateSelectionGeneration(draws);
                        renderer_.points.updateUniforms(commandBuffer, draws);
                        renderer_.vectors.syncLayers(commandBuffer,
                                                     document->layers);
                        renderer_.vectors.updateUniforms(commandBuffer,
                                                         vectorDraws);
                        // Uploads, texture creation, layer pruning, and uniform
                        // updates must all complete before beginPass().
                        auto rasterFrame =
                            renderer_.rasterDraws(rhi(),
                                                  commandBuffer,
                                                  rasterCapture.input.camera,
                                                  rasterCapture.layers,
                                                  rasterPlan,
                                                  frameExecutor_.rasters());
                        rasterSelectedTiles_ =
                            rasterPlan.statistics.selectedTiles;
                        rasterFinestLevel_ = rasterPlan.statistics.finestLevel;
                        rasterCoarsestLevel_ =
                            rasterPlan.statistics.coarsestLevel;
                        rasterCoverageIncomplete_ =
                            rasterPlan.statistics.coverageIncomplete;
                        rasterUploadedTiles_ += rasterFrame.uploaded;
                        rasterDrawnTiles_ = rasterFrame.draws.size();
                        rasterSurfaceDrawnTiles_ = rasterFrame.surfaceDrawn;
                        std::erase_if(
                            rasterDecodeStates_,
                            [&rasterPlan](const auto &entry) {
                                return std::ranges::find(
                                           rasterPlan.retainedLayers,
                                           entry.first) ==
                                       rasterPlan.retainedLayers.end();
                            });
                        rasterDraws = std::move(rasterFrame.draws);
                        rasterNeedsAnotherFrame =
                            rasterFrame.requiresContinuation;
                        renderer_.rasters.updateUniforms(commandBuffer,
                                                         rasterDraws);
                        submitPendingPick(
                            commandBuffer, draws, plan->decodedLeases);
                        if (eyeDomeLightingActive_) {
                            const auto clip =
                                inputController_->camera().clipPlanes();
                            renderer_.edl.updateUniforms(
                                commandBuffer,
                                static_cast<float>(clip.nearPlane),
                                static_cast<float>(clip.farPlane),
                                viewportSettings_.depthEnhancement.radius,
                                viewportSettings_.depthEnhancement.strength);
                        }
                        renderer_.record(commandBuffer,
                                         renderTarget(),
                                         viewportSettings_,
                                         eyeDomeLightingActive_,
                                         draws,
                                         vectorDraws,
                                         rasterDraws);
                        commandTime = elapsedSince(commandStart);
                    },
                .complete =
                    [&](bool continuation) {
                        planNeedsAnotherFrame |= continuation;
                    },
            });
        } else {
            outOfFrustumLayerIds_.clear();
            snapshotTime = elapsedSince(snapshotStart);
            const auto commandStart = std::chrono::steady_clock::now();
            if (eyeDomeLightingActive_) {
                const auto clip = inputController_->camera().clipPlanes();
                renderer_.edl.updateUniforms(
                    commandBuffer,
                    static_cast<float>(clip.nearPlane),
                    static_cast<float>(clip.farPlane),
                    viewportSettings_.depthEnhancement.radius,
                    viewportSettings_.depthEnhancement.strength);
            }
            renderer_.record(commandBuffer,
                             renderTarget(),
                             viewportSettings_,
                             eyeDomeLightingActive_,
                             draws,
                             vectorDraws,
                             rasterDraws);
            commandTime = elapsedSince(commandStart);
        }
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }

#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    // Throttle the residency probe: a stalled load can render thousands of
    // frames, and one line per frame per layer would bury the pattern.
    bool probeDue = false;
    if (probeResidency_) {
        saturatingAccumulate(probeDroppedUploads_,
                             renderer_.uploads.frameMetrics().droppedUploads);
        saturatingAccumulate(probeDroppedBytes_,
                             renderer_.uploads.frameMetrics().droppedBytes);
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextResidencyProbe_) {
            probeDue = true;
            nextResidencyProbe_ = now + std::chrono::seconds(1);
            qInfo().noquote()
                << QStringLiteral(
                       "[probe] residency frame=%1 budget_current=%2 "
                       "budget_total=%3 protected_mib=%4 resident_mib=%5/%6 "
                       "evictions=%7 dropped_uploads=%8 dropped_mib=%9 "
                       "pending_display=%10")
                       .arg(frameExecutor_.telemetry().frameCount())
                       .arg(pointBudget_.current())
                       .arg(pointBudget_.total())
                       .arg(renderer_.uploads.frameMetrics().protectedBytes /
                            (std::uint64_t{1024} * 1024))
                       .arg(renderer_.uploads.residentBytes() /
                            (std::uint64_t{1024} * 1024))
                       .arg(renderer_.uploads.residencyByteBudget() /
                            (std::uint64_t{1024} * 1024))
                       .arg(renderer_.uploads.evictionCount())
                       .arg(probeDroppedUploads_)
                       .arg(probeDroppedBytes_ / (std::uint64_t{1024} * 1024))
                       .arg(pendingDisplayReadyLayerIds_.size());
        }
    }
#endif

    for (auto pending = pendingFirstFrameLayerIds_.begin();
         pending != pendingFirstFrameLayerIds_.end();) {
        const PointCloudLayerId layerId = *pending;
        const SceneDocumentSnapshotPtr &document =
            sceneSnapshotCache_.document();
        const std::optional<PointCloudLayerSnapshot> pointLayer =
            document ? document->layer(layerId) : std::nullopt;
        if (!pointLayer) {
            pending = pendingFirstFrameLayerIds_.erase(pending);
            continue;
        }
        if (renderer_.uploads.residentPointCount(layerId) == 0) {
            ++pending;
            continue;
        }
        publishLoadProgress({
            .layerId = layerId,
            .bindingGeneration = pointLayer->bindingGeneration,
            .stage = RenderLoadStage::FirstFrameReady,
            .completed = renderer_.uploads.residentPointCount(layerId),
            .total = renderer_.uploads.residentPointCount(layerId),
        });
        pending = pendingFirstFrameLayerIds_.erase(pending);
    }

    for (auto pending = pendingDisplayReadyLayerIds_.begin();
         pending != pendingDisplayReadyLayerIds_.end();) {
        const PointCloudLayerId layerId = *pending;
        const SceneDocumentSnapshotPtr &document =
            sceneSnapshotCache_.document();
        const std::optional<PointCloudLayerSnapshot> pointLayer =
            document ? document->layer(layerId) : std::nullopt;
        const PointDatasetRuntimeSnapshot *snapshot =
            sceneSnapshotCache_.snapshot(layerId);
        // A layer outside the frustum is deliberately given no uploads, so it
        // can never acquire GPU residency. Requiring residency from it blocks
        // display readiness forever, which in turn never retires the load job.
        // Once its import is complete there is nothing further the renderer
        // will do for it while it is off screen, so it is as ready as it gets.
        const bool offScreen = outOfFrustumLayerIds_.contains(layerId);
        if (!pointLayer || !snapshot || !snapshot->loadingComplete ||
            (renderer_.uploads.residentPointCount(layerId) == 0 &&
             !offScreen)) {
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
            if (probeResidency_ && probeDue) {
                qInfo().noquote()
                    << QStringLiteral(
                           "[probe] display-blocked layer=%1 reason=%2 "
                           "resident=%3")
                           .arg(layerId.value())
                           .arg(!snapshot ? QStringLiteral("no-snapshot")
                                : !snapshot->loadingComplete
                                    ? QStringLiteral("importing")
                                : renderer_.uploads.residentPointCount(
                                      layerId) == 0 &&
                                        !offScreen
                                    ? QStringLiteral("no-resident-block")
                                    : QStringLiteral("ready"))
                           .arg(renderer_.uploads.residentPointCount(layerId));
            }
#endif
            ++pending;
            continue;
        }

        const std::uint64_t completed =
            renderer_.uploads.residentPointCount(layerId);
        const std::uint64_t total = snapshot->hierarchical
                                        ? snapshot->sourcePointCount
                                        : snapshot->retainedFlatPointCount;
        publishLoadProgress({
            .layerId = layerId,
            .bindingGeneration = pointLayer->bindingGeneration,
            .stage = RenderLoadStage::DisplayReady,
            .completed = completed,
            .total = total,
        });
        pending = pendingDisplayReadyLayerIds_.erase(pending);
    }

    std::uint64_t submittedPoints = 0;
    for (const BlockDraw &draw : draws) {
        submittedPoints = saturatingAdd(
            submittedPoints, static_cast<std::uint64_t>(draw.pointCount));
    }
    frameExecutor_.telemetry().recordFrame({
        .frameTime = duration,
        .outputWidth = renderTarget()->pixelSize().width(),
        .outputHeight = renderTarget()->pixelSize().height(),
        .snapshotTime = snapshotTime,
        .selectionTime = selectionTime,
        .uploadTime = uploadTime,
        .commandTime = commandTime,
        .requestedPoints = requestedPoints,
        .selectedPoints = selectedPoints,
        .submittedPoints = submittedPoints,
        .drawCalls = static_cast<std::uint64_t>(draws.size()),
        .vectorLayersDrawn = vectorLayersDrawn,
        .vectorDrawCalls = vectorDrawCalls,
        .visibleBlocks = visibleBlocks,
        .culledBlocks = culledBlocks,
        .visibleLayerCount = visibleLayerCount,
        .coveredLayerCount = coveredLayerCount,
        .rootOnlyLayerCount = rootOnlyLayerCount,
        .framePlanReused = framePlanReused,
        .includedUploads = uploaded > 0,
        .includedPick = frameStartedWithPick || renderer_.picker.inFlight(),
        .uploadedPointBytes = renderer_.uploads.frameMetrics().uploadedBytes,
        .protectedGpuPointBytes =
            renderer_.uploads.frameMetrics().protectedBytes,
        .pendingUploadBytes = renderer_.uploads.frameMetrics().pendingBytes,
        .uploadOperations = renderer_.uploads.frameMetrics().uploadOperations,
        .resourceUpdateBatches =
            renderer_.uploads.frameMetrics().resourceUpdateBatches,
        .uniformUpdateOperations =
            renderer_.points.uniformUpdateOperationCount(),
    });

    const bool continueRendering = shouldContinueRendering({
        .movement = inputController_->state().hasMovement() ||
                    inputController_->dragging(),
        .pendingPick = inputController_->state().pendingPick().has_value(),
        .pickReadback = renderer_.picker.inFlight(),
        .pendingUploads = uploadsNeedAnotherFrame || planNeedsAnotherFrame ||
                          rasterNeedsAnotherFrame,
        .sceneInvalidation = sceneInvalidationPending_ ||
                             (qualificationCameraPath_ &&
                              !qualificationCameraPath_->current().finalFrame),
        .pendingSmokeFrames =
            smokeTest_ && frameExecutor_.telemetry().frameCount() < 3,
    });
    // Event-driven rendering may not submit another frame for a long time.
    // Publish the settling frame even inside the normal sampling interval so
    // diagnostics capture the final selected/submitted counts and frame total.
    publishMetrics(continuousMetricsEnabled_ || !continueRendering ||
                   frameExecutor_.telemetry().frameCount() == 1);

    if (qualificationCameraPath_) {
        if (qualificationCameraPath_->advance()) {
            applyQualificationCameraFrame();
        } else {
            qualificationCameraPath_.reset();
        }
    }

    if (smokeTest_ && frameExecutor_.telemetry().frameCount() >= 3) {
        QTimer::singleShot(0, QCoreApplication::instance(), [] {
            QCoreApplication::exit(0);
        });
        return;
    }

    frameTimingContinuous_ = continueRendering;
    if (continueRendering) {
        requestRender();
    }
}

void RenderViewportWidget::queueSceneInvalidation()
{
    sceneInvalidationPending_ = true;
    requestRender();
}

void RenderViewportWidget::refreshDocumentState(
    const std::vector<LayerFrameState> &layers)
{
    std::uint64_t sourcePoints = 0;
    std::uint64_t retainedFlatPoints = 0;
    for (const LayerFrameState &layer : layers) {
        saturatingAccumulate(sourcePoints, layer.snapshot->sourcePointCount);
        saturatingAccumulate(retainedFlatPoints,
                             layer.snapshot->retainedFlatPointCount);
    }
    pointCount_ = sourcePoints;
    frameExecutor_.telemetry().setSourceTotals(sourcePoints,
                                               retainedFlatPoints);
    const PointBudgetUpdate budget = frameExecutor_.points().pointBudgetUpdate(
        layers,
        runtimeBudget_.decodedPointBytes,
        renderer_.uploads.residencyByteBudget(),
        pointBudget_.current());
    if (pointBudget_.total() != budget.total) {
        pointBudget_.setTotal(budget.total);
    }
    if (budget.current) {
        pointBudget_.setCurrent(*budget.current);
    }
    std::vector<PointCloudLayerId> currentLayerIds;
    currentLayerIds.reserve(layers.size());
    for (const LayerFrameState &layer : layers) {
        currentLayerIds.push_back(layer.layerId);
    }
    const SceneSnapshotCache::LayerChanges layerChanges =
        sceneSnapshotCache_.reconcileVisibleLayers(currentLayerIds);
    const std::unordered_set<PointCloudLayerId> addedLayerIds(
        layerChanges.added.begin(), layerChanges.added.end());
    for (const LayerFrameState &layer : layers) {
        if (!addedLayerIds.contains(layer.layerId)) {
            continue;
        }
        pendingFirstFrameLayerIds_.insert(layer.layerId);
        pendingDisplayReadyLayerIds_.insert(layer.layerId);
        publishLoadProgress({
            .layerId = layer.layerId,
            .bindingGeneration = layer.bindingGeneration,
            .stage = RenderLoadStage::Preparing,
            .completed = 0,
            .total = layer.snapshot->sourcePointCount,
        });
    }
    for (auto pending = pendingFirstFrameLayerIds_.begin();
         pending != pendingFirstFrameLayerIds_.end();) {
        if (sceneSnapshotCache_.containsVisibleLayer(*pending)) {
            ++pending;
        } else {
            pending = pendingFirstFrameLayerIds_.erase(pending);
        }
    }
    for (auto pending = pendingDisplayReadyLayerIds_.begin();
         pending != pendingDisplayReadyLayerIds_.end();) {
        if (sceneSnapshotCache_.containsVisibleLayer(*pending)) {
            ++pending;
        } else {
            pending = pendingDisplayReadyLayerIds_.erase(pending);
        }
    }
}

std::uint64_t RenderViewportWidget::framePointBudget(
    const std::vector<LayerFrameState> &layers) const noexcept
{
    const bool settledFlat =
        !layers.empty() &&
        std::ranges::all_of(layers, [](const LayerFrameState &layer) {
            return !layer.snapshot->hierarchical &&
                   layer.snapshot->loadingComplete;
        });
    const bool interacting =
        inputController_->state().hasMovement() || inputController_->dragging();
    return settledFlat && !interacting ? pointBudget_.total()
                                       : pointBudget_.current();
}

FrameCamera RenderViewportWidget::currentFrameCamera() const
{
    const QSize outputSize = renderTarget()->pixelSize();
    const int width = std::max(1, outputSize.width());
    const int height = std::max(1, outputSize.height());
    const double aspect =
        static_cast<double>(width) / static_cast<double>(height);
    const auto clip = inputController_->camera().clipPlanes();
    const Vec3d eye = inputController_->camera().position();
    const Vec3d forward = inputController_->camera().forward();
    const Vec3d up = inputController_->camera().up();
    const Vec3d right = inputController_->camera().right();
    const double halfVertical =
        inputController_->camera().orthographicScale() * 0.5;
    return {
        .eye = eye,
        .forward = forward,
        .up = up,
        .right = right,
        .culler = orthographic_
                      ? FrustumCuller::fromOrthographic(eye,
                                                        forward,
                                                        up,
                                                        right,
                                                        halfVertical,
                                                        aspect,
                                                        clip.nearPlane,
                                                        clip.farPlane)
                      : FrustumCuller::fromCamera(
                            eye,
                            forward,
                            up,
                            right,
                            NavigationCamera::verticalFieldOfViewDegrees,
                            aspect,
                            clip.nearPlane,
                            clip.farPlane),
        .outputWidth = width,
        .outputHeight = height,
        .nearPlane = clip.nearPlane,
        .farPlane = clip.farPlane,
        .verticalFovDegrees = NavigationCamera::verticalFieldOfViewDegrees,
        .orthographicScale = inputController_->camera().orthographicScale(),
        .orthographic = orthographic_,
    };
}

void RenderViewportWidget::updateSelectionGeneration(
    const std::vector<BlockDraw> &draws)
{
    std::vector<DrawSignature> signature;
    signature.reserve(draws.size());
    for (const BlockDraw &draw : draws) {
        signature.push_back({
            .block = draw.block.get(),
            .pointCount = draw.pointCount,
            .idBase = draw.idBase,
        });
    }
    if (signature != drawSignature_) {
        drawSignature_ = std::move(signature);
        ++selectionGeneration_;
    }
}

std::vector<BlockDraw>
RenderViewportWidget::pickCandidates(const std::vector<BlockDraw> &draws,
                                     const PixelPosition position,
                                     const QSize targetSize) const
{
    const auto clip = inputController_->camera().clipPlanes();
    const double largestPointSize = largestDrawPointSizePixels(draws);
    const ScreenPickVolume volume =
        makeScreenPickVolume(inputController_->camera().position(),
                             inputController_->camera().forward(),
                             inputController_->camera().right(),
                             inputController_->camera().up(),
                             NavigationCamera::verticalFieldOfViewDegrees,
                             targetSize.width(),
                             targetSize.height(),
                             static_cast<double>(position.x),
                             static_cast<double>(position.y),
                             std::max(4.0, largestPointSize * 0.5 + 1.0),
                             clip.nearPlane,
                             clip.farPlane);
    if (!volume.valid()) {
        return draws;
    }

    std::vector<BlockDraw> candidates;
    candidates.reserve(draws.size());
    for (const BlockDraw &draw : draws) {
        if (draw.block &&
            intersectsScreenPickVolume(volume, draw.block->bounds)) {
            candidates.push_back(draw);
        }
    }
    std::ranges::stable_sort(
        candidates, [&volume](const BlockDraw &left, const BlockDraw &right) {
            return screenPickDistance(volume, left.block->bounds) <
                   screenPickDistance(volume, right.block->bounds);
        });
    return candidates;
}

void RenderViewportWidget::submitPendingPick(
    QRhiCommandBuffer *commandBuffer,
    const std::vector<BlockDraw> &draws,
    const std::vector<PointCloudNodePayloadPtr> &leases)
{
    if (renderer_.picker.inFlight()) {
        return;
    }
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    const std::uint64_t documentRevision = document ? document->revision : 0;
    const auto request = inputController_->state().takePendingPick(
        inputController_->camera().revision(),
        documentRevision,
        selectionGeneration_);
    if (!request) {
        return;
    }
    const QSize targetSize = renderTarget()->pixelSize();
    const double scaleX = static_cast<double>(targetSize.width()) /
                          static_cast<double>(std::max(1, width()));
    const double scaleY = static_cast<double>(targetSize.height()) /
                          static_cast<double>(std::max(1, height()));
    const QPoint pixelPosition(
        static_cast<int>(std::lround(request->position.x * scaleX)),
        static_cast<int>(std::lround(request->position.y * scaleY)));
    const std::vector<BlockDraw> candidates = pickCandidates(
        draws, {pixelPosition.x(), pixelPosition.y()}, targetSize);
    RenderPickingTelemetry picking{
        .inputBlocks = static_cast<std::uint64_t>(draws.size()),
        .candidateBlocks = static_cast<std::uint64_t>(candidates.size()),
    };
    for (const BlockDraw &draw : draws) {
        saturatingAccumulate(picking.inputPoints, draw.pointCount);
    }
    for (const BlockDraw &draw : candidates) {
        saturatingAccumulate(picking.candidatePoints, draw.pointCount);
    }
    frameExecutor_.telemetry().setPicking(picking);
    const bool measurementPick = request->kind == PickKind::MeasureHover ||
                                 request->kind == PickKind::MeasureCommit;
    const int readbackRadius =
        measurementPick
            ? std::clamp(
                  static_cast<int>(std::ceil(10.0 * std::max(scaleX, scaleY))),
                  1,
                  PointPicker::pickTargetEdge / 2 - 1)
            : 2;
    const auto pickGuard = FramePickGuard::capture(
        {sessionGeneration_, document, runtimeSnapshot_});
    pickGpuProtection_ = currentGpuProtection_;
    renderer_.picker.record(
        commandBuffer,
        renderer_.points.shaderBindings(),
        candidates,
        renderer_.points.uniformStride(),
        pixelPosition,
        targetSize,
        readbackRadius,
        [this,
         delivery = std::weak_ptr<bool>(pickDelivery_),
         pickGuard,
         request = *request,
         candidates,
         leases](const std::optional<std::uint32_t> pointId) {
            // Keep the cache's payload lease (and accounting) alive until the
            // asynchronous CPU consumer has finished resolving the point.
            static_cast<void>(leases);
            const auto live = delivery.lock();
            if (!live || !*live) {
                return;
            }
            pickGpuProtection_.clear();
            if (!pickGuard.valid({sessionGeneration_,
                                  sceneSnapshotCache_.document(),
                                  runtimeSnapshot_})) {
                inputController_->state().completeInFlight();
                return;
            }
            // The immutable document guards root/color publication
            // as well as binding replacement for every pick kind.
            resolvePick(request, pointId, candidates);
        });
}

void RenderViewportWidget::resolvePick(
    const PickRequest request,
    const std::optional<std::uint32_t> pointId,
    const std::vector<BlockDraw> &draws)
{
    if (!inputController_->state().isCurrentPick(request)) {
        return;
    }
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    const std::uint64_t documentRevision = document ? document->revision : 0;
    if (request.kind == PickKind::MeasureCommit) {
        inputController_->state().completeInFlight();
        if (activeTool_ == ViewportTool::Measure) {
            measurementController_.acceptCommitResult(
                resolvePickedPoint(pointId, draws), measurementRevisions());
            updateMeasurementOverlay();
            requestRender();
            const PixelPosition position =
                measurementController_.state().hoverPosition;
            scheduleMeasurementHover(QPoint(position.x, position.y));
        }
        return;
    }
    if (request.kind == PickKind::MeasureHover) {
        inputController_->state().completeInFlight();
        if (activeTool_ == ViewportTool::Measure &&
            measurementController_.acceptHoverResult(
                request.serial,
                {request.cameraRevision,
                 request.documentRevision,
                 request.selectionGeneration},
                measurementRevisions(),
                resolvePickedPoint(pointId, draws))) {
            updateMeasurementOverlay();
        }
        return;
    }
    if (request.cameraRevision != inputController_->camera().revision() ||
        request.documentRevision != documentRevision ||
        request.selectionGeneration != selectionGeneration_) {
        inputController_->state().markStale(request);
        requestRender();
        return;
    }
    inputController_->state().completeInFlight();

    if (request.kind == PickKind::Raw) {
        auto completion = std::move(rawPickCompletion_);
        rawPickCompletion_ = {};
        if (completion) {
            completion(pointId);
        }
        return;
    }

    const std::optional<Vec3d> target = resolvePickedPoint(pointId, draws);

    if (request.kind == PickKind::Pivot) {
        if (target) {
            inputController_->camera().setPivot(*target);
        }
    } else if (target) {
        inputController_->camera().dollyToward(*target, request.wheelUnits);
    } else {
        inputController_->camera().dollyForward(request.wheelUnits);
    }
    requestRender();
}

std::optional<Vec3d> RenderViewportWidget::resolvePickedPoint(
    const std::optional<std::uint32_t> pointId,
    const std::vector<BlockDraw> &draws) const
{
    if (!pointId) {
        return std::nullopt;
    }
    for (const BlockDraw &draw : draws) {
        if (*pointId >= draw.idBase &&
            *pointId < draw.idBase + draw.pointCount) {
            return decodeBlockPosition(
                *draw.block, draw.block->points[*pointId - draw.idBase]);
        }
    }
    return std::nullopt;
}

void RenderViewportWidget::queueMeasurementHover(const PixelPosition position)
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    if (activeTool_ != ViewportTool::Measure || !document ||
        !document->hasPointCloudLayers() || renderer_.picker.inFlight()) {
        return;
    }
    const std::uint64_t serial =
        inputController_->state().queueMeasureHover(position);
    if (serial != 0) {
        measurementController_.recordHoverQueued(
            serial, MeasurementController::Clock::now());
        requestRender();
    }
}

void RenderViewportWidget::scheduleMeasurementHover(const QPoint position)
{
    if (activeTool_ != ViewportTool::Measure) {
        return;
    }
    const auto delay = measurementController_.scheduleHover(
        {position.x(), position.y()}, MeasurementController::Clock::now());
    if (measurementHoverTimer_ && delay) {
        measurementHoverTimer_->start(static_cast<int>(delay->count()));
    }
}

void RenderViewportWidget::clearMeasurement()
{
    measurementController_.clear();
    updateMeasurementOverlay();
}

void RenderViewportWidget::updateMeasurementOverlay()
{
    if (!measurementOverlay_) {
        return;
    }
    measurementController_.invalidateHover(measurementRevisions());
    const MeasurementViewState &state = measurementController_.state();
    measurementOverlay_->setState(
        activeTool_,
        inputController_->camera(),
        QPoint(state.hoverPosition.x, state.hoverPosition.y),
        state.hover,
        state.anchor,
        state.measurement);
}

MeasurementRevisions RenderViewportWidget::measurementRevisions() const noexcept
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    return {
        inputController_->camera().revision(),
        document ? document->revision : 0,
        selectionGeneration_,
    };
}

void RenderViewportWidget::updateKeyboardNavigation()
{
    const double deltaSeconds = NavigationInputState::boundedDeltaSeconds(
        static_cast<double>(navigationTimer_.restart()) / 1000.0);
    applyKeyboardNavigation(deltaSeconds);
}

void RenderViewportWidget::applyKeyboardNavigation(const double deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) {
        return;
    }
    const Vec3d inputDirection = inputController_->state().movementDirection();
    if (length(inputDirection) == 0.0) {
        return;
    }

    if (mapView_ && inputDirection.y != 0.0) {
        constexpr double zoomWheelUnitsPerSecond = 4.0;
        inputController_->camera().dollyForward(
            -inputDirection.y * zoomWheelUnitsPerSecond *
            inputController_->state().speedMultiplier() * deltaSeconds);
    }

    const Vec3d worldDirection =
        mapView_
            ? normalized(inputController_->camera().right() * inputDirection.x +
                         inputController_->camera().up() * inputDirection.z)
            : normalized(inputController_->camera().right() * inputDirection.x +
                         NavigationCamera::worldUp * inputDirection.y +
                         inputController_->camera().forward() *
                             inputDirection.z);
    if (length(worldDirection) == 0.0) {
        return;
    }
    const double speed = mapView_
                             ? inputController_->camera().orthographicScale() *
                                   inputController_->state().speedMultiplier()
                             : inputController_->camera().movementSpeed(
                                   inputController_->state().speedMultiplier());
    inputController_->camera().translate(worldDirection * speed * deltaSeconds);
}

void RenderViewportWidget::publishMetrics(const bool force)
{
    std::optional<RenderTelemetrySnapshot> telemetry =
        frameExecutor_.telemetry().takeSnapshot(
            RenderTelemetryAccumulator::Clock::now(), force);
    if (!telemetry) {
        return;
    }

    if (metricsCallback_) {
        std::uint64_t decodedPointBytes = 0;
        std::uint64_t decodedResidentPoints = 0;
        const SceneDocumentSnapshotPtr &document =
            sceneSnapshotCache_.document();
        if (document) {
            for (const SceneSnapshotLayer &layer : document->layers) {
                const auto *point =
                    std::get_if<PointCloudLayerSnapshotState>(&layer.payload);
                if (!point) {
                    continue;
                }
                const PointDatasetRuntimePtr scene =
                    runtimeSnapshot_
                        ? runtimeSnapshot_->point(point->descriptor.sourceId,
                                                  layer.bindingGeneration)
                        : PointDatasetRuntimePtr{};
                if (!scene) {
                    continue;
                }
                decodedPointBytes = saturatingAdd(
                    decodedPointBytes, scene->decodedResidentBytes());
                if (layer.visible) {
                    saturatingAccumulate(decodedResidentPoints,
                                         scene->decodedResidentPoints());
                }
            }
        }
        const ProcessMemoryMetrics memory = processMemoryMetrics();
        const RasterStreamerMetrics rasterStreamerMetrics =
            frameExecutor_.rasters().metrics();
        telemetry->backend = {
            .deviceName = deviceName_.toStdString(),
            .requestedBackend =
                std::string(graphicsApiName(requestedGraphicsApi_)),
            .selectedBackend = backendApiName(api()).toStdString(),
            .timingSource = timingSource_.toStdString(),
            .gpuValidationEnabled = gpuValidationEnabled_,
        };
        telemetry->upload.uniformDrawCapacity =
            static_cast<std::uint64_t>(renderer_.points.uniformDrawCapacity());
        telemetry->upload.uniformCapacityGrowthCount =
            renderer_.points.uniformCapacityGrowthCount();
        telemetry->residency = {
            .gpuVectorBytes = renderer_.vectors.gpuBytes(),
            .rasterCpuBytes = rasterStreamerMetrics.cpuBytes,
            .rasterCpuBudgetBytes = frameExecutor_.rasters().cpuByteBudget(),
            .rasterCpuPeakBytes = rasterStreamerMetrics.cpuPeakBytes,
            .rasterGpuBytes = renderer_.rasters.gpuBytes(),
            .rasterHeightGpuBytes = renderer_.rasters.heightGpuBytes(),
            .rasterGpuBudgetBytes = renderer_.rasters.gpuByteBudget(),
            .rasterGpuPeakBytes = renderer_.rasters.peakGpuBytes(),
            .rasterTilesRequested = rasterStreamerMetrics.requested,
            .rasterTilesCompleted = rasterStreamerMetrics.completed,
            .rasterTilesCancelled = rasterStreamerMetrics.cancelled,
            .rasterTilesFailed = rasterStreamerMetrics.failed,
            .rasterCacheEvictions = rasterStreamerMetrics.cacheEvictions,
            .rasterUploadedTiles = rasterUploadedTiles_,
            .rasterResidentTiles = renderer_.rasters.residentTileCount(),
            .rasterSelectedTiles = rasterSelectedTiles_,
            .rasterDrawnTiles = rasterDrawnTiles_,
            .rasterSurfaceDrawnTiles = rasterSurfaceDrawnTiles_,
            .rasterSurfaceTriangles =
                static_cast<std::uint64_t>(rasterSurfaceDrawnTiles_) *
                rasterSurfaceGridCellsPerSide * rasterSurfaceGridCellsPerSide *
                2ULL,
            .rasterPendingReads = rasterStreamerMetrics.pending,
            .rasterFinestLevel = rasterFinestLevel_,
            .rasterCoarsestLevel = rasterCoarsestLevel_,
            .rasterCoverageIncomplete = rasterCoverageIncomplete_,
            .gpuResidentPoints = renderer_.uploads.residentPointCount(),
            .gpuPointBudgetBytes = renderer_.uploads.residencyByteBudget(),
            .gpuPointBytes = renderer_.uploads.residentBytes(),
            .peakGpuPointBytes = renderer_.uploads.peakResidentBytes(),
            .gpuCacheEvictions = renderer_.uploads.evictionCount(),
            .processResidentBytes = memory.residentBytes,
            .peakProcessResidentBytes = memory.peakResidentBytes,
        };
        telemetry->decode = {
            .decodedResidentPoints = decodedResidentPoints,
            .decodedPointBytes = decodedPointBytes,
        };
        RenderMetrics metrics = projectRenderMetrics(*telemetry);
        if (qualificationCameraPath_) {
            const QualificationCameraFrame frame =
                qualificationCameraPath_->current();
            metrics.qualificationPhase =
                QString::fromLatin1(qualificationFramePhaseName(frame.phase));
            metrics.qualificationFrameIndex = frame.frameIndex;
            metrics.qualificationFrameCount = frame.totalFrames;
            metrics.qualificationFrame = true;
            metrics.qualificationFinalFrame = frame.finalFrame;
        }
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
        if (profileRendering_) {
            qInfo().noquote()
                << QStringLiteral(
                       "Render profile: backend=%1 frame=%2 frame_ms=%3 "
                       "snapshot_ms=%4 selection_ms=%5 upload_ms=%6 "
                       "command_ms=%7 requested=%8 selected=%9 submitted=%10 "
                       "draws=%11 pick_blocks=%12/%13 pick_points=%14/%15 "
                       "gpu_bytes=%16/%17 gpu_budget=%18 gpu_evictions=%19 "
                       "cpu_bytes=%20/%21")
                       .arg(metrics.selectedBackend)
                       .arg(metrics.submittedFrameCount)
                       .arg(metrics.frameMilliseconds, 0, 'f', 3)
                       .arg(metrics.sceneSnapshotMilliseconds, 0, 'f', 3)
                       .arg(metrics.selectionMilliseconds, 0, 'f', 3)
                       .arg(metrics.uploadMilliseconds, 0, 'f', 3)
                       .arg(metrics.commandRecordingMilliseconds, 0, 'f', 3)
                       .arg(metrics.requestedPoints)
                       .arg(metrics.selectedPoints)
                       .arg(metrics.submittedPoints)
                       .arg(metrics.drawCalls)
                       .arg(metrics.pickCandidateBlocks)
                       .arg(metrics.pickInputBlocks)
                       .arg(metrics.pickCandidatePoints)
                       .arg(metrics.pickInputPoints)
                       .arg(metrics.gpuPointBytes)
                       .arg(metrics.peakGpuPointBytes)
                       .arg(metrics.gpuPointBudgetBytes)
                       .arg(metrics.gpuCacheEvictions)
                       .arg(metrics.decodedPointBytes)
                       .arg(metrics.decodedPointBudgetBytes);
        }
#endif
        QTimer::singleShot(0, this, [this, metrics] {
            if (metricsCallback_) {
                metricsCallback_(metrics);
            }
        });
    }
}

void RenderViewportWidget::publishLoadProgress(RenderLoadProgress progress)
{
    if (loadProgressCallback_) {
        QTimer::singleShot(0, this, [this, progress] {
            if (loadProgressCallback_) {
                loadProgressCallback_(progress);
            }
        });
    }
}

void RenderViewportWidget::fail(const QString &message)
{
    if (failed_) {
        return;
    }
    failed_ = true;
    qCritical().noquote() << QStringLiteral("Renderer: %1").arg(message);
    if (failureCallback_) {
        failureCallback_(message);
    }
    if (smokeTest_) {
        QTimer::singleShot(0, QCoreApplication::instance(), [] {
            QCoreApplication::exit(1);
        });
    }
}

void RenderViewportWidget::releaseResources()
{
    inputController_->state().cancelPicks();
    rawPickCompletion_ = {};
    pickGpuProtection_.clear();
    currentGpuProtection_.clear();
    renderer_.releaseResources();
    rasterDrawnTiles_ = 0;
    rasterSurfaceDrawnTiles_ = 0;
    rasterSurfaceCapability_ = RasterSurfaceCapability::Unknown;
    rasterSurfaceCapabilityReason_.clear();
    if (rasterSurfaceCapabilityCallback_) {
        rasterSurfaceCapabilityCallback_(rasterSurfaceCapability_, {});
    }
    resourceRhi_ = nullptr;
}

void RenderViewportWidget::mousePressEvent(QMouseEvent *event)
{
    if (!inputController_->mousePressEvent(event)) {
        QRhiWidget::mousePressEvent(event);
    }
}

void RenderViewportWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!inputController_->mouseDoubleClickEvent(event)) {
        QRhiWidget::mouseDoubleClickEvent(event);
    }
}

void RenderViewportWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!inputController_->mouseMoveEvent(event)) {
        QRhiWidget::mouseMoveEvent(event);
    }
}

void RenderViewportWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (!inputController_->mouseReleaseEvent(event)) {
        QRhiWidget::mouseReleaseEvent(event);
    }
}

void RenderViewportWidget::wheelEvent(QWheelEvent *event)
{
    if (!inputController_->wheelEvent(event)) {
        QRhiWidget::wheelEvent(event);
    }
}

void RenderViewportWidget::keyPressEvent(QKeyEvent *event)
{
    if (!inputController_->keyPressEvent(event)) {
        QRhiWidget::keyPressEvent(event);
    }
}

void RenderViewportWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (!inputController_->keyReleaseEvent(event)) {
        QRhiWidget::keyReleaseEvent(event);
    }
}

void RenderViewportWidget::focusOutEvent(QFocusEvent *event)
{
    if (!inputController_->focusOutEvent(event)) {
        QRhiWidget::focusOutEvent(event);
    }
}

void RenderViewportWidget::resizeEvent(QResizeEvent *event)
{
    QRhiWidget::resizeEvent(event);
    if (measurementOverlay_) {
        measurementOverlay_->setGeometry(rect());
        measurementOverlay_->raise();
    }
    updateMeasurementOverlay();
}

std::unique_ptr<RenderViewport>
createRenderViewport(const bool smokeTest,
                     const std::uint64_t gpuByteBudget,
                     const GraphicsApi graphicsApi,
                     const bool enableGpuValidation,
                     PointColorMapCatalogSnapshotPtr colorMaps)
{
    return std::make_unique<RenderViewportWidget>(smokeTest,
                                                  gpuByteBudget,
                                                  graphicsApi,
                                                  enableGpuValidation,
                                                  std::move(colorMaps));
}

} // namespace pci
