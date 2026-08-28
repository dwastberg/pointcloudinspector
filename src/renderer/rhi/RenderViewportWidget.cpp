#include "renderer/rhi/RenderViewportWidget_p.h"

#include "foundation/CheckedArithmetic.h"
#include "platform/ProcessMemory.h"
#include "renderer/PointColorMapAtlas.h"
#include "renderer/planning/FrustumCuller.h"
#include "renderer/planning/PointSizePolicy.h"
#include "renderer/rhi/BackendPolicy.h"
#include "scene/RasterLayerDisplay.h"

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
    , uploadScheduler_(gpuByteBudget)
    , colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
    , pointCloudRenderer_(colorMaps_)
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
    const QPointer<RenderViewportWidget> viewport(this);
    sceneSnapshotCache_.setCallbacks(
        [viewport] {
            if (viewport) {
                viewport->queueSceneInvalidation();
            }
        },
        [viewport](std::function<void()> callback) {
            if (!viewport) {
                return false;
            }
            return QMetaObject::invokeMethod(
                viewport.data(),
                [viewport, callback = std::move(callback)] {
                    if (viewport) {
                        callback();
                    }
                },
                Qt::QueuedConnection);
        });
    rasterTileStreamer_.setWakeCallback([viewport] {
        if (!viewport) {
            return;
        }
        static_cast<void>(QMetaObject::invokeMethod(
            viewport.data(),
            [viewport] {
                if (viewport) {
                    viewport->queueSceneInvalidation();
                }
            },
            Qt::QueuedConnection));
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
    // Workers may otherwise enqueue a wake while the QObject and its retained
    // scene sources are being dismantled. shutdown() is idempotent and the
    // streamer's member destructor repeats it defensively.
    rasterTileStreamer_.shutdown();
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

void RenderViewportWidget::setDocument(SceneDocumentSnapshotPtr document,
                                       const bool frameVisibleLayers)
{
    if (!document) {
        throw std::invalid_argument("document must not be null");
    }
    const bool replacingDocument =
        static_cast<bool>(sceneSnapshotCache_.document());
    sceneSnapshotCache_.setDocument(std::move(document), true);
    pendingFirstFrameLayerIds_.clear();
    pendingDisplayReadyLayerIds_.clear();
    publishedResidentPointCounts_.clear();
    hierarchyLayerErrors_.clear();
    pointFrameCoordinator_.clear();
    drawSignature_.clear();
    ++selectionGeneration_;
    telemetry_.setPicking({});
    if (replacingDocument) {
        uploadScheduler_.retainLayers({});
    }
    const SceneDocumentSnapshotPtr &currentDocument =
        sceneSnapshotCache_.document();
    pointCount_ = currentDocument->visibleExpectedPointCount;
    telemetry_.setSourceTotals(pointCount_, 0);
    pointBudget_ = AdaptivePointBudget(std::max<std::uint64_t>(pointCount_, 1));
    input_.cancelPicks();
    rawPickCompletion_ = {};
    if (replacingDocument) {
        clearMeasurement();
    }

    if (frameVisibleLayers) {
        this->frameVisibleLayers();
    }
    requestRender();
}

void RenderViewportWidget::updateDocument(SceneDocumentSnapshotPtr document)
{
    if (!document) {
        throw std::invalid_argument("document must not be null");
    }
    sceneSnapshotCache_.setDocument(std::move(document), false);
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
    camera_.frameTopDown(1.15);
    requestRender();
}

bool RenderViewportWidget::isOrthographic() const noexcept
{
    return orthographic_;
}

void RenderViewportWidget::setOrthographic(const bool enabled)
{
    if (orthographic_ == enabled) {
        return;
    }
    orthographic_ = enabled;
    camera_.setOrthographic(enabled);
    requestRender();
}

void RenderViewportWidget::frameBounds(const std::optional<Bounds3d> &bounds,
                                       const double distanceMultiplier)
{
    if (bounds && bounds->valid() && bounds->maximumExtent() > 0.0) {
        const auto center = bounds->center();
        camera_.setScene({center[0], center[1], center[2]},
                         bounds->maximumExtent());
    } else {
        camera_.setScene({0.0, 0.0, 0.0}, 2.0);
    }
    camera_.frameScene(distanceMultiplier);
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
    return uploadScheduler_.residencyByteBudget();
}

void RenderViewportWidget::setGpuByteBudget(const std::uint64_t byteBudget)
{
    if (uploadScheduler_.residencyByteBudget() == byteBudget) {
        return;
    }
    uploadScheduler_.setResidencyByteBudget(byteBudget);
    queueSceneInvalidation();
}

void RenderViewportWidget::setRasterByteBudgets(
    const std::uint64_t cpuByteBudget, const std::uint64_t gpuByteBudget)
{
    // Nothing is on screen at this point in the frame, so no tile needs
    // protecting: a decrease evicts purely by recency and the caches come back
    // under their limits before the next admission compares against them.
    rasterTileStreamer_.setCpuByteBudget(cpuByteBudget, {});
    rasterLayerRenderer_.setGpuByteBudget(gpuByteBudget, {});
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
    pendingMeasureClick_ = false;
    dragMode_ = DragMode::None;
    input_.cancelPicks();
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
    telemetry_.makeNextSnapshotDue(RenderTelemetryAccumulator::Clock::now());
    requestRender();
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
            telemetry_.makeNextSnapshotDue(previousCpuFrame_);
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
        !eyeDomeLightingPass_.matchesPointTarget(rhi(), outputSize)) {
        // The point pipeline references the EDL target's render-pass
        // descriptor. Destroy that pipeline before replacing the target.
        pointCloudRenderer_.releaseResources();
        vectorLayerRenderer_.releaseResources();
        rasterLayerRenderer_.releaseResources();
        eyeDomeLightingPass_.releaseResources();
        eyeDomeLightingActive_ = false;
    }

    if (viewportSettings_.depthEnhancement.enabled) {
        eyeDomeLightingActive_ = eyeDomeLightingPass_.ensureResources(
            rhi(), outputSize, renderTarget()->renderPassDescriptor());
    } else {
        // Switch the point pipeline away from the EDL descriptor before
        // releasing the target that owns it.
        pointCloudRenderer_.ensureResources(
            rhi(), renderTarget()->renderPassDescriptor());
        eyeDomeLightingPass_.releaseResources();
        eyeDomeLightingActive_ = false;
    }

    QRhiRenderPassDescriptor *pointRenderPass =
        renderTarget()->renderPassDescriptor();
    if (eyeDomeLightingActive_) {
        pointRenderPass = eyeDomeLightingPass_.pointRenderPassDescriptor();
    }
    pointCloudRenderer_.ensureResources(rhi(), pointRenderPass);
    // Overlays always record into the widget target: when EDL is active this
    // is the post-composite pass, where republished point depth remains usable.
    rasterLayerRenderer_.ensureResources(
        rhi(), renderTarget()->renderPassDescriptor());
    const RasterSurfaceCapability rasterCapability =
        rasterLayerRenderer_.surfaceSupported()
            ? RasterSurfaceCapability::Supported
            : RasterSurfaceCapability::Unsupported;
    const QString rasterReason = QString::fromStdString(
        rasterLayerRenderer_.surfaceCapabilityReason());
    if (rasterCapability != rasterSurfaceCapability_ ||
        rasterReason != rasterSurfaceCapabilityReason_) {
        rasterSurfaceCapability_ = rasterCapability;
        rasterSurfaceCapabilityReason_ = rasterReason;
        if (rasterSurfaceCapabilityCallback_) {
            rasterSurfaceCapabilityCallback_(rasterSurfaceCapability_,
                                             rasterSurfaceCapabilityReason_);
        }
    }
    vectorLayerRenderer_.ensureResources(
        rhi(), renderTarget()->renderPassDescriptor());
    pointPicker_.ensureResources(rhi(), pointCloudRenderer_.shaderBindings());
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
RenderViewportWidget::RasterFrameResult
RenderViewportWidget::streamRasterTiles(QRhiCommandBuffer *commandBuffer)
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    rasterSelectedTiles_ = 0;
    rasterDrawnTiles_ = 0;
    rasterSurfaceDrawnTiles_ = 0;
    // Finest starts above every valid level so the first selection sets it;
    // starting at zero would make the reported minimum permanently zero.
    rasterFinestLevel_ = std::numeric_limits<std::uint32_t>::max();
    rasterCoarsestLevel_ = 0;
    rasterCoverageIncomplete_ = false;

    // An empty document is deliberately not an early return: releasing removed
    // sources and pruning per-layer state is exactly what a removed layer
    // needs, and skipping it strands that layer's in-flight reads in a
    // completion queue nothing ever drains.
    const std::vector<RasterLayer> layers =
        document ? document->rasterLayers() : std::vector<RasterLayer>{};

    const FrameCamera frame = currentFrameCamera();
    const QMatrix4x4 viewProjection = frameViewProjection(frame);

    // A completed read names its source, not its layer.
    struct UploadTarget {
        SceneLayerId layerId;
        bool nearest = false;
    };

    std::vector<SceneLayerId> retained;
    std::vector<RasterSourceId> liveSources;
    std::vector<RasterCacheKey> protectedTiles;
    std::vector<RasterLayerDraw> draws;
    std::vector<RasterPendingUpload> pending;
    std::vector<RasterCacheKey> decodedUploads;
    std::unordered_map<RasterSourceId, UploadTarget> uploadTargets;
    // Plans outlive the loop because reconciliation is frame-global; see
    // RasterFrameLayer.
    std::vector<std::pair<std::size_t, RasterLodPlan>> plans;
    plans.reserve(layers.size());
    const std::size_t visibleRasterLayers = static_cast<std::size_t>(
        std::ranges::count_if(layers, [](const RasterLayer &layer) {
            return layer.visible && static_cast<bool>(layer.data);
        }));
    const bool surfacePayloads =
        rasterLayerRenderer_.surfaceSupported() &&
        std::ranges::any_of(layers, [](const RasterLayer &layer) {
            return layer.visible && layer.data &&
                   layer.data->metadata().elevation.available;
        });
    const std::size_t perLayerTileCapacity =
        visibleRasterLayers == 0
            ? 1
            : std::max<std::size_t>(
                  1,
                  rasterLayerRenderer_
                          .tileCapacity(
                              surfacePayloads
                                  ? RasterTilePayloadProfile::RenderElevation
                                  : RasterTilePayloadProfile::ColorOnly) /
                      visibleRasterLayers);

    for (std::size_t index = 0; index < layers.size(); ++index) {
        const RasterLayer &layer = layers[index];
        retained.push_back(layer.id);
        if (!layer.data) {
            continue;
        }
        liveSources.push_back(layer.data->sourceId);
        const RasterLayerMetadata &metadata = layer.data->metadata();
        RasterDecodeState &decodeState = rasterDecodeStates_[layer.id];
        if (!decodeState.parameters ||
            decodeState.generation != layer.renderGeneration) {
            decodeState.generation = layer.renderGeneration;
            decodeState.parameters =
                resolveRasterDecodeParameters(layer, *colorMaps_);
        }
        // Registered even while hidden. A read that finished before the layer
        // was hidden is still queued for upload, and dropping it would strand
        // the tile CPU-resident but never uploaded: the planner would not ask
        // for it again, because the decoded cache already holds it.
        uploadTargets.insert_or_assign(
            layer.data->sourceId,
            UploadTarget{
                .layerId = layer.id,
                .nearest = metadata.defaultDisplay.sampleKind ==
                           RasterSampleKind::Categorical,
            });
        if (!layer.visible) {
            continue;
        }

        const RasterTilePayloadProfile profile =
            rasterLayerRenderer_.surfaceSupported() &&
                    metadata.elevation.available
                ? RasterTilePayloadProfile::RenderElevation
                : RasterTilePayloadProfile::ColorOnly;
        const bool surface =
            rasterLayerRenderer_.surfaceSupported() &&
            rasterEffectiveRenderMode(metadata,
                                      layer.style,
                                      layer.elevationStatus,
                                      layer.exactElevationRange) ==
                RasterRenderMode::Surface;

        RasterLodPlanInput input;
        input.layer = layer;
        input.camera = frame;
        input.previousSelection = previousRasterSelection_[layer.id];
        // The renderer cache is shared by every raster layer. Giving each
        // planner the full capacity lets N overlapping layers pin N times the
        // budget; divide the target allowance across the visible set and keep
        // the renderer's reserved quarter for fallbacks and uploads.
        input.gpuCapacityTiles = perLayerTileCapacity;
        // Residency is answered against this layer's own cache keys, so two
        // layers cannot be mistaken for one another.
        const RasterSourceId sourceId = layer.data->sourceId;
        const std::uint64_t generation = layer.renderGeneration;
        input.gpuResident =
            [this, sourceId, generation, profile](const RasterTileKey key) {
                return rasterLayerRenderer_.gpuResident(
                    RasterCacheKey{sourceId, generation, key, profile});
            };
        input.cpuResident =
            [this, sourceId, generation, profile](const RasterTileKey key) {
                return rasterTileStreamer_.cpuResident(
                    RasterCacheKey{sourceId, generation, key, profile});
            };
        input.unavailable =
            [this, sourceId, generation, profile](const RasterTileKey key) {
                return rasterTileStreamer_.failed(
                    RasterCacheKey{sourceId, generation, key, profile});
            };

        const RasterLodPlan &plan =
            plans.emplace_back(index, planRasterTiles(input)).second;
        previousRasterSelection_[layer.id] = plan.selected;
        rasterSelectedTiles_ += plan.selected.size();
        rasterCoverageIncomplete_ =
            rasterCoverageIncomplete_ || plan.coverageIncomplete;
        for (const RasterTileKey key : plan.selected) {
            rasterFinestLevel_ = std::min(rasterFinestLevel_, key.levelIndex);
            rasterCoarsestLevel_ =
                std::max(rasterCoarsestLevel_, key.levelIndex);
        }

        for (const RasterTileKey key : plan.protectedTiles) {
            protectedTiles.push_back({sourceId, generation, key, profile});
        }
        for (const RasterTileKey key : plan.decodedUploads) {
            decodedUploads.push_back({sourceId, generation, key, profile});
        }
        // plan.draw is sorted fine-to-coarse. Reverse only this layer's range
        // so its fallback parents paint first, while the outer layer loop
        // preserves document painter order across layers with different LODs.
        for (auto drawKey = plan.draw.rbegin(); drawKey != plan.draw.rend();
             ++drawKey) {
            const RasterTileKey key = *drawKey;
            const RasterCacheKey cacheKey{sourceId,
                                          generation,
                                          key,
                                          profile};
            if (surface) {
                const auto residualRange =
                    rasterLayerRenderer_.tileElevationResidualRange(cacheKey);
                if (!residualRange) {
                    // Render-elevation tiles with no valid height also have
                    // fully invalid source alpha. Avoid submitting thousands
                    // of triangles only to discard every fragment.
                    continue;
                }
                const RasterBasePixelRect rect = rasterTileBasePixelRect(
                    metadata.levels[key.levelIndex],
                    key,
                    metadata.width,
                    metadata.height);
                const std::array<Vec3d, 4> footprint{
                    rasterPixelToWorld(metadata.geoTransform,
                                       rect.minimumPixel,
                                       rect.minimumLine),
                    rasterPixelToWorld(metadata.geoTransform,
                                       rect.maximumPixel,
                                       rect.minimumLine),
                    rasterPixelToWorld(metadata.geoTransform,
                                       rect.maximumPixel,
                                       rect.maximumLine),
                    rasterPixelToWorld(metadata.geoTransform,
                                       rect.minimumPixel,
                                       rect.maximumLine),
                };
                Bounds3d tileBounds;
                tileBounds.minimum = {
                    std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max(),
                    (metadata.elevation.anchor + residualRange->minimum) *
                            layer.style.verticalExaggeration +
                        layer.style.zOffset,
                };
                tileBounds.maximum = {
                    std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest(),
                    (metadata.elevation.anchor + residualRange->maximum) *
                            layer.style.verticalExaggeration +
                        layer.style.zOffset,
                };
                if (tileBounds.minimum[2] > tileBounds.maximum[2]) {
                    std::swap(tileBounds.minimum[2], tileBounds.maximum[2]);
                }
                for (const Vec3d corner : footprint) {
                    tileBounds.minimum[0] =
                        std::min(tileBounds.minimum[0], corner.x);
                    tileBounds.minimum[1] =
                        std::min(tileBounds.minimum[1], corner.y);
                    tileBounds.maximum[0] =
                        std::max(tileBounds.maximum[0], corner.x);
                    tileBounds.maximum[1] =
                        std::max(tileBounds.maximum[1], corner.y);
                }
                if (!frame.culler.intersects(tileBounds)) {
                    continue;
                }
            }
            RasterQuadTransform quad =
                rasterTileQuadTransform(metadata, layer.style, key, frame.eye);
            RasterLayerDraw draw;
            draw.layerId = layer.id;
            draw.tileKey = cacheKey;
            draw.mode = surface ? RasterDrawMode::Surface
                                : RasterDrawMode::Flat;
            draw.surfaceRole =
                std::ranges::find(plan.selected, key) == plan.selected.end()
                    ? RasterSurfaceDrawRole::Fallback
                    : RasterSurfaceDrawRole::Detail;
            draw.transparent = layer.style.opacity < 0.999F;
            const QVector4D clipOrigin =
                viewProjection * QVector4D(static_cast<float>(quad.origin.x),
                                           static_cast<float>(quad.origin.y),
                                           static_cast<float>(quad.origin.z),
                                           1.0F);
            const QVector4D clipEdgeU =
                viewProjection *
                    QVector4D(static_cast<float>(quad.origin.x + quad.edgeU.x),
                              static_cast<float>(quad.origin.y + quad.edgeU.y),
                              static_cast<float>(quad.origin.z + quad.edgeU.z),
                              1.0F) -
                clipOrigin;
            const QVector4D clipEdgeV =
                viewProjection *
                    QVector4D(static_cast<float>(quad.origin.x + quad.edgeV.x),
                              static_cast<float>(quad.origin.y + quad.edgeV.y),
                              static_cast<float>(quad.origin.z + quad.edgeV.z),
                              1.0F) -
                clipOrigin;
            const auto copyClip = [](std::array<float, 4> &destination,
                                     const QVector4D value) {
                destination = {value.x(), value.y(), value.z(), value.w()};
            };
            copyClip(draw.uniform.clipTopLeft, clipOrigin);
            copyClip(draw.uniform.clipTopRight, clipOrigin + clipEdgeU);
            copyClip(draw.uniform.clipBottomLeft, clipOrigin + clipEdgeV);
            copyClip(draw.uniform.clipBottomRight,
                     clipOrigin + clipEdgeU + clipEdgeV);
            // The level's last row and column are short, so the UV rect must
            // come from the tile's own valid extent rather than assuming a
            // full tile; otherwise an edge tile samples its replicated gutter
            // as if it were image content.
            const RasterTileExtent extent =
                rasterTileValidExtent(metadata.levels[key.levelIndex], key);
            draw.uniform.uvRect =
                rasterTileUvRect(static_cast<std::uint16_t>(extent.width),
                                 static_cast<std::uint16_t>(extent.height));
            draw.uniform.opacity = layer.style.opacity;
            if (surface) {
                quad.origin.z = metadata.elevation.anchor *
                                        layer.style.verticalExaggeration +
                                    layer.style.zOffset - frame.eye.z;
                std::copy_n(viewProjection.constData(),
                            draw.surfaceUniform.viewProjection.size(),
                            draw.surfaceUniform.viewProjection.begin());
                draw.surfaceUniform.origin = {
                    static_cast<float>(quad.origin.x),
                    static_cast<float>(quad.origin.y),
                    static_cast<float>(quad.origin.z),
                    0.0F,
                };
                draw.surfaceUniform.edgeU = {
                    static_cast<float>(quad.edgeU.x),
                    static_cast<float>(quad.edgeU.y),
                    static_cast<float>(quad.edgeU.z),
                    0.0F,
                };
                draw.surfaceUniform.edgeV = {
                    static_cast<float>(quad.edgeV.x),
                    static_cast<float>(quad.edgeV.y),
                    static_cast<float>(quad.edgeV.z),
                    0.0F,
                };
                draw.surfaceUniform.uvRect = draw.uniform.uvRect;
                draw.surfaceUniform.tileTexels = {
                    static_cast<float>(extent.width),
                    static_cast<float>(extent.height),
                    static_cast<float>(rasterStoredTilePixels),
                    static_cast<float>(rasterTileGutter),
                };
                draw.surfaceUniform.heightParams = {
                    static_cast<float>(layer.style.verticalExaggeration),
                    layer.style.opacity,
                    0.5F / 255.0F,
                    layer.style.surfaceShadingStrength,
                };
                draw.surfaceUniform.shadingParams = {
                    0.45F,
                    0.55F,
                    static_cast<float>(rasterSurfaceGridCellsPerSide),
                    0.0F,
                };
            }
            draws.push_back(draw);
        }
    }

    if (rasterFinestLevel_ == std::numeric_limits<std::uint32_t>::max()) {
        rasterFinestLevel_ = 0;
    }

    // One reconciliation for the whole frame. Per-layer calls would make each
    // layer's cancellation sweep drop every other layer's queued reads.
    std::vector<RasterFrameLayer> frameLayers;
    frameLayers.reserve(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto planned =
            std::ranges::find_if(plans, [index](const auto &entry) {
                return entry.first == index;
            });
        const auto decode = rasterDecodeStates_.find(layers[index].id);
        frameLayers.push_back(RasterFrameLayer{
            .plan = planned == plans.end() ? nullptr : &planned->second,
            .layer = &layers[index],
            .decode = decode == rasterDecodeStates_.end()
                          ? nullptr
                          : decode->second.parameters,
            .profile = layers[index].data &&
                               rasterLayerRenderer_.surfaceSupported() &&
                               layers[index].data->metadata().elevation.available
                           ? RasterTilePayloadProfile::RenderElevation
                           : RasterTilePayloadProfile::ColorOnly,
        });
    }
    rasterTileStreamer_.reconcile(frameLayers);

    // A source the document no longer owns releases its decoded tiles, queued
    // reads, remembered failures, and GPU textures. Without this its in-flight
    // reads complete into a queue nothing drains, holding bytes outside every
    // accounted cache while the counters read zero.
    for (const RasterSourceId sourceId : knownRasterSources_) {
        if (std::ranges::find(liveSources, sourceId) != liveSources.end()) {
            continue;
        }
        rasterTileStreamer_.releaseSource(sourceId);
        rasterLayerRenderer_.releaseSource(sourceId);
    }
    knownRasterSources_ = std::move(liveSources);

    static_cast<void>(rasterTileStreamer_.drainCompletions(protectedTiles));
    rasterTileStreamer_.requeueReadyUploads(decodedUploads);
    const std::vector<RasterCacheKey> readyUploads =
        rasterTileStreamer_.takeReadyUploads(rasterMaximumFrameUploads);
    std::vector<RasterCacheKey> attemptedUploads;
    attemptedUploads.reserve(readyUploads.size());
    for (const RasterCacheKey &key : readyUploads) {
        const auto target = uploadTargets.find(key.sourceId);
        if (target == uploadTargets.end()) {
            continue;
        }
        const RasterTileData *tile = rasterTileStreamer_.tile(key);
        if (!tile) {
            continue;
        }
        pending.push_back(RasterPendingUpload{
            .key = key,
            .layerId = target->second.layerId,
            .tile = tile,
            .nearest = target->second.nearest,
        });
        attemptedUploads.push_back(key);
    }
    rasterLayerRenderer_.retainLayers(retained);
    const std::size_t uploaded = rasterLayerRenderer_.uploadPending(
        commandBuffer, pending, protectedTiles, rasterFrameUploadBytes);
    rasterUploadedTiles_ += uploaded;
    std::vector<RasterCacheKey> retryUploads;
    retryUploads.reserve(attemptedUploads.size() - uploaded);
    for (const RasterCacheKey &key : attemptedUploads) {
        if (!rasterLayerRenderer_.gpuResident(key)) {
            retryUploads.push_back(key);
        }
    }
    rasterTileStreamer_.requeueReadyUploads(retryUploads);
    rasterDrawnTiles_ = draws.size();
    rasterSurfaceDrawnTiles_ = static_cast<std::size_t>(std::ranges::count_if(
        draws, [](const RasterLayerDraw &draw) {
            return draw.mode == RasterDrawMode::Surface;
        }));
    std::erase_if(previousRasterSelection_, [&retained](const auto &entry) {
        return std::ranges::find(retained, entry.first) == retained.end();
    });
    std::erase_if(rasterDecodeStates_, [&retained](const auto &entry) {
        return std::ranges::find(retained, entry.first) == retained.end();
    });
    return RasterFrameResult{
        .draws = std::move(draws),
        // Plans are built before upload. A successful upload becomes drawable
        // only after the next plan observes it as GPU-resident.
        .requiresContinuation = uploaded > 0,
    };
}

void RenderViewportWidget::recordScene(
    QRhiCommandBuffer *commandBuffer,
    const std::vector<BlockDraw> &draws,
    const std::span<const VectorLayerDraw> vectorDraws,
    const std::span<const RasterLayerDraw> rasterDraws)
{
    const QColor clear =
        QColor::fromRgbF(viewportSettings_.backgroundColor.red,
                         viewportSettings_.backgroundColor.green,
                         viewportSettings_.backgroundColor.blue,
                         1.0F);
    const QRhiDepthStencilClearValue depthClear{1.0F, 0};
    QRhiRenderTarget *pointTarget =
        eyeDomeLightingActive_ ? eyeDomeLightingPass_.pointRenderTarget()
                               : renderTarget();

    // Overlays record into whichever target holds the published depth:
    //   EDL off: begin widget pass -> points -> rasters -> vectors -> end
    //   EDL on:  begin EDL pass    -> points -> end
    //            begin widget pass -> composite -> rasters -> vectors -> end
    // Rasters receive no eye-dome lighting, which is why they draw after the
    // composite rather than alongside the points.
    commandBuffer->beginPass(pointTarget, clear, depthClear);
    pointCloudRenderer_.recordDraws(commandBuffer, pointTarget, draws);
    if (!eyeDomeLightingActive_) {
        rasterLayerRenderer_.recordDraws(
            commandBuffer, pointTarget, rasterDraws);
        vectorLayerRenderer_.recordDraws(
            commandBuffer, pointTarget, vectorDraws);
    }
    commandBuffer->endPass();

    if (eyeDomeLightingActive_) {
        commandBuffer->beginPass(renderTarget(), clear, depthClear);
        eyeDomeLightingPass_.recordComposite(commandBuffer, renderTarget());
        rasterLayerRenderer_.recordDraws(
            commandBuffer, renderTarget(), rasterDraws);
        vectorLayerRenderer_.recordDraws(
            commandBuffer, renderTarget(), vectorDraws);
        commandBuffer->endPass();
    }
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
    if (!pointCloudRenderer_.ready() || !vectorLayerRenderer_.ready() ||
        !rasterLayerRenderer_.ready()) {
        return;
    }

    sceneInvalidationPending_ = false;
    updateKeyboardNavigation();
    const auto duration = frameDuration(commandBuffer);
    if (telemetry_.frameCount() > 0 && duration.count() > 0.0) {
        const bool delayedGpuWork = timingSource_ == QStringLiteral("GPU") &&
                                    telemetry_.specialFrameCooldownActive();
        pointBudget_.update({
            .frameTime = duration,
            .gpuTiming = timingSource_ == QStringLiteral("GPU"),
            .includedUploads =
                telemetry_.previousFrameIncludedUploads() || delayedGpuWork,
            .includedPick =
                telemetry_.previousFrameIncludedPick() || delayedGpuWork,
            .submittedPoints = telemetry_.submittedPoints(),
        });
    }
    telemetry_.advanceSpecialFrameCooldown();

    std::vector<BlockDraw> draws;
    std::vector<VectorLayerDraw> vectorDraws;
    std::vector<RasterLayerDraw> rasterDraws;
    std::uint64_t selectedPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    std::uint64_t requestedPoints = 0;
    std::uint64_t vectorLayersDrawn = 0;
    std::uint64_t vectorDrawCalls = 0;
    bool framePlanReused = false;
    std::size_t uploaded = 0;
    bool uploadsNeedAnotherFrame = false;
    bool planNeedsAnotherFrame = false;
    bool rasterNeedsAnotherFrame = false;
    const bool frameStartedWithPick =
        input_.pendingPick().has_value() || pointPicker_.inFlight();
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
            std::vector<PointCloudLayerId> retainedLayerIds;
            const Bounds3d documentColorBounds =
                document->bounds.value_or(Bounds3d{});
            const SceneSnapshotCache::RefreshResult snapshotRefresh =
                sceneSnapshotCache_.refresh();
            for (const PointCloudLayerId layerId :
                 snapshotRefresh.invalidatedRootPayloads) {
                uploadScheduler_.invalidateNode(layerId, rootPointCloudNode);
            }
            for (const PointCloudLayerId layerId :
                 snapshotRefresh.invalidatedColors) {
                uploadScheduler_.invalidateLayer(layerId);
            }
            for (const SceneLayer &sceneLayer : document->layers) {
                const auto *point =
                    std::get_if<PointCloudLayerState>(&sceneLayer.payload);
                if (!point) {
                    continue;
                }
                const PointCloudLayer layer{
                    .id = sceneLayer.id,
                    .scene = point->scene,
                    .visible = sceneLayer.visible,
                    .colorMode = point->colorMode,
                    .classificationFilter = point->classificationFilter,
                    .rasterColors = point->rasterColors,
                    .colorGeneration = point->colorGeneration,
                };
                retainedLayerIds.push_back(layer.id);
                if (!layer.visible) {
                    continue;
                }
                const PointCloudSceneSnapshot *cached =
                    sceneSnapshotCache_.snapshot(layer.id);
                if (!cached) {
                    continue;
                }
                layers.push_back({
                    .layer = layer,
                    .snapshot = cached,
                    .colorRange =
                        effectivePointColorRange(layer.colorMode,
                                                 documentColorBounds,
                                                 cached->scalarRanges),
                });
            }
            snapshotTime = elapsedSince(snapshotStart);

            uploadScheduler_.beginFrame(telemetry_.nextFrameNumber());
            uploadScheduler_.retainLayers(retainedLayerIds);
            refreshDocumentState(layers);
            const std::uint64_t currentFramePointBudget =
                framePointBudget(layers);
            requestedPoints = currentFramePointBudget;

            const auto selectionStart = std::chrono::steady_clock::now();
            const bool allFlat =
                std::ranges::all_of(layers, [](const LayerFrameState &layer) {
                    return !layer.snapshot->hierarchical;
                });
            const PointFrameResult pointFrame = pointFrameCoordinator_.plan({
                .document = sceneSnapshotCache_.document(),
                .layers = layers,
                .camera = currentFrameCamera(),
                .cameraRevision = camera_.revision(),
                .framePointBudget = currentFramePointBudget,
                .gpuByteBudget = uploadScheduler_.residencyByteBudget(),
                .resident =
                    [this](const PointFrameBlockKey &key) {
                        return uploadScheduler_.bufferFor({
                                   .layerId = key.layerId,
                                   .nodeId = key.nodeId,
                                   .nodeBlockIndex = key.nodeBlockIndex,
                                   .sceneBlockId = key.sceneBlockId,
                               }) != nullptr;
                    },
            });
            const PointFramePlan *plan = pointFrame.plan.get();
            framePlanReused = pointFrame.reused;
            selectionTime = elapsedSince(selectionStart);
            outOfFrustumLayerIds_ = plan->outOfFrustumLayerIds;
            selectedPoints = plan->selectedPoints;
            visibleBlocks = plan->visibleBlocks;
            culledBlocks = plan->culledBlocks;
            visibleLayerCount = plan->visibleLayerCount;
            coveredLayerCount = plan->coveredLayerCount;
            planNeedsAnotherFrame = plan->requiresContinuation;
            std::unordered_map<PointCloudLayerId, std::string> currentErrors;
            currentErrors.reserve(plan->layerErrors.size());
            for (const PointFrameLayerError &error : plan->layerErrors) {
                currentErrors.emplace(error.layerId, error.message);
                const auto previous = hierarchyLayerErrors_.find(error.layerId);
                if (previous == hierarchyLayerErrors_.end() ||
                    previous->second != error.message) {
                    qWarning().noquote()
                        << QStringLiteral(
                               "Point-cloud layer %1 was dropped from the "
                               "frame: %2")
                               .arg(error.layerId.value())
                               .arg(QString::fromStdString(error.message));
                }
            }
            hierarchyLayerErrors_ = std::move(currentErrors);

            const auto uploadStart = std::chrono::steady_clock::now();
            {
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
                const DebugMarkScope uploadMarker(
                    commandBuffer, QByteArrayLiteral("Point uploads"));
#endif
                uploaded =
                    uploadScheduler_.uploadPending(rhi(),
                                                   commandBuffer,
                                                   plan->uploads,
                                                   plan->protectedGpuBlocks);
            }
            uploadTime = elapsedSince(uploadStart);
            const bool uploadsRemain = std::ranges::any_of(
                plan->uploads, [this](const UploadBlock &upload) {
                    return !uploadScheduler_.bufferFor(upload.key);
                });
            // Continue only while the previous frame made progress. An upload
            // that cannot fit the configured residency budget must not create
            // a permanent busy loop. Hierarchical selection is evaluated
            // before uploads, so even the final upload needs one follow-up
            // frame to make the node drawable or commit a refinement.
            uploadsNeedAnotherFrame =
                uploaded > 0 && (uploadsRemain || !allFlat);
            for (const LayerFrameState &layer : layers) {
                const std::uint64_t resident =
                    uploadScheduler_.residentPointCount(layer.layer.id);
                const std::uint64_t previous =
                    publishedResidentPointCounts_[layer.layer.id];
                publishedResidentPointCounts_[layer.layer.id] = resident;
                const std::uint64_t total =
                    layer.snapshot->hierarchical
                        ? layer.snapshot->sourcePointCount
                        : layer.snapshot->retainedFlatPointCount;
                if (resident != previous && resident < total) {
                    publishLoadProgress({
                        .layerId = layer.layer.id,
                        .stage = RenderLoadStage::Uploading,
                        .completed = resident,
                        .total = total,
                    });
                }
            }
            const auto commandStart = std::chrono::steady_clock::now();
            draws = buildDrawList(*plan);
            vectorDraws = buildVectorDrawList();
            vectorLayersDrawn = vectorDraws.size();
            for (const VectorLayerDraw &draw : vectorDraws) {
                if (!draw.layer.data) {
                    continue;
                }
                vectorDrawCalls =
                    saturatingAdd(vectorDrawCalls,
                                  static_cast<std::uint64_t>(
                                      draw.layer.data->fillBatches.size()));
                vectorDrawCalls = saturatingAdd(
                    vectorDrawCalls,
                    std::uint64_t{draw.layer.data->segments.empty() ? 0U : 1U});
                vectorDrawCalls = saturatingAdd(
                    vectorDrawCalls,
                    std::uint64_t{draw.layer.data->markers.empty() ? 0U : 1U});
            }
            updateSelectionGeneration(draws);
            pointCloudRenderer_.updateUniforms(commandBuffer, draws);
            vectorLayerRenderer_.syncLayers(commandBuffer, document->layers);
            vectorLayerRenderer_.updateUniforms(commandBuffer, vectorDraws);
            // Uploads, texture creation, layer pruning, and uniform updates
            // must all complete before beginPass().
            RasterFrameResult rasterFrame = streamRasterTiles(commandBuffer);
            rasterDraws = std::move(rasterFrame.draws);
            rasterNeedsAnotherFrame = rasterFrame.requiresContinuation;
            rasterLayerRenderer_.updateUniforms(commandBuffer, rasterDraws);
            submitPendingPick(commandBuffer, draws);
            if (eyeDomeLightingActive_) {
                const auto clip = camera_.clipPlanes();
                eyeDomeLightingPass_.updateUniforms(
                    commandBuffer,
                    static_cast<float>(clip.nearPlane),
                    static_cast<float>(clip.farPlane),
                    viewportSettings_.depthEnhancement.radius,
                    viewportSettings_.depthEnhancement.strength);
            }
            recordScene(commandBuffer, draws, vectorDraws, rasterDraws);
            commandTime = elapsedSince(commandStart);
        } else {
            outOfFrustumLayerIds_.clear();
            snapshotTime = elapsedSince(snapshotStart);
            const auto commandStart = std::chrono::steady_clock::now();
            if (eyeDomeLightingActive_) {
                const auto clip = camera_.clipPlanes();
                eyeDomeLightingPass_.updateUniforms(
                    commandBuffer,
                    static_cast<float>(clip.nearPlane),
                    static_cast<float>(clip.farPlane),
                    viewportSettings_.depthEnhancement.radius,
                    viewportSettings_.depthEnhancement.strength);
            }
            recordScene(commandBuffer, draws, vectorDraws, rasterDraws);
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
                             uploadScheduler_.frameMetrics().droppedUploads);
        saturatingAccumulate(probeDroppedBytes_,
                             uploadScheduler_.frameMetrics().droppedBytes);
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
                       .arg(telemetry_.frameCount())
                       .arg(pointBudget_.current())
                       .arg(pointBudget_.total())
                       .arg(uploadScheduler_.frameMetrics().protectedBytes /
                            (std::uint64_t{1024} * 1024))
                       .arg(uploadScheduler_.residentBytes() /
                            (std::uint64_t{1024} * 1024))
                       .arg(uploadScheduler_.residencyByteBudget() /
                            (std::uint64_t{1024} * 1024))
                       .arg(uploadScheduler_.evictionCount())
                       .arg(probeDroppedUploads_)
                       .arg(probeDroppedBytes_ / (std::uint64_t{1024} * 1024))
                       .arg(pendingDisplayReadyLayerIds_.size());
        }
    }
#endif

    for (auto pending = pendingFirstFrameLayerIds_.begin();
         pending != pendingFirstFrameLayerIds_.end();) {
        const PointCloudLayerId layerId = *pending;
        if (uploadScheduler_.residentPointCount(layerId) == 0) {
            ++pending;
            continue;
        }
        publishLoadProgress({
            .layerId = layerId,
            .stage = RenderLoadStage::FirstFrameReady,
            .completed = uploadScheduler_.residentPointCount(layerId),
            .total = uploadScheduler_.residentPointCount(layerId),
        });
        pending = pendingFirstFrameLayerIds_.erase(pending);
    }

    for (auto pending = pendingDisplayReadyLayerIds_.begin();
         pending != pendingDisplayReadyLayerIds_.end();) {
        const PointCloudLayerId layerId = *pending;
        const PointCloudSceneSnapshot *snapshot =
            sceneSnapshotCache_.snapshot(layerId);
        // A layer outside the frustum is deliberately given no uploads, so it
        // can never acquire GPU residency. Requiring residency from it blocks
        // display readiness forever, which in turn never retires the load job.
        // Once its import is complete there is nothing further the renderer
        // will do for it while it is off screen, so it is as ready as it gets.
        const bool offScreen = outOfFrustumLayerIds_.contains(layerId);
        if (!snapshot || !snapshot->loadingComplete ||
            (uploadScheduler_.residentPointCount(layerId) == 0 &&
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
                                : uploadScheduler_.residentPointCount(
                                      layerId) == 0 &&
                                        !offScreen
                                    ? QStringLiteral("no-resident-block")
                                    : QStringLiteral("ready"))
                           .arg(uploadScheduler_.residentPointCount(layerId));
            }
#endif
            ++pending;
            continue;
        }

        const std::uint64_t completed =
            uploadScheduler_.residentPointCount(layerId);
        const std::uint64_t total = snapshot->hierarchical
                                        ? snapshot->sourcePointCount
                                        : snapshot->retainedFlatPointCount;
        publishLoadProgress({
            .layerId = layerId,
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
    telemetry_.recordFrame({
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
        .framePlanReused = framePlanReused,
        .includedUploads = uploaded > 0,
        .includedPick = frameStartedWithPick || pointPicker_.inFlight(),
        .uploadedPointBytes = uploadScheduler_.frameMetrics().uploadedBytes,
        .protectedGpuPointBytes =
            uploadScheduler_.frameMetrics().protectedBytes,
        .pendingUploadBytes = uploadScheduler_.frameMetrics().pendingBytes,
        .uploadOperations = uploadScheduler_.frameMetrics().uploadOperations,
        .resourceUpdateBatches =
            uploadScheduler_.frameMetrics().resourceUpdateBatches,
        .uniformUpdateOperations =
            pointCloudRenderer_.uniformUpdateOperationCount(),
    });

    const bool continueRendering = shouldContinueRendering({
        .movement = input_.hasMovement() || dragMode_ != DragMode::None,
        .pendingPick = input_.pendingPick().has_value(),
        .pickReadback = pointPicker_.inFlight(),
        .pendingUploads = uploadsNeedAnotherFrame || planNeedsAnotherFrame ||
                          rasterNeedsAnotherFrame,
        .sceneInvalidation = sceneInvalidationPending_,
        .pendingSmokeFrames = smokeTest_ && telemetry_.frameCount() < 3,
    });
    // Event-driven rendering may not submit another frame for a long time.
    // Publish the settling frame even inside the normal sampling interval so
    // diagnostics capture the final selected/submitted counts and frame total.
    publishMetrics(continuousMetricsEnabled_ || !continueRendering ||
                   telemetry_.frameCount() == 1);

    if (smokeTest_ && telemetry_.frameCount() >= 3) {
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
    telemetry_.setSourceTotals(sourcePoints, retainedFlatPoints);
    const PointBudgetUpdate budget = pointFrameCoordinator_.pointBudgetUpdate(
        layers, uploadScheduler_.residencyByteBudget(), pointBudget_.current());
    if (pointBudget_.total() != budget.total) {
        pointBudget_.setTotal(budget.total);
    }
    if (budget.current) {
        pointBudget_.setCurrent(*budget.current);
    }
    std::vector<PointCloudLayerId> currentLayerIds;
    currentLayerIds.reserve(layers.size());
    for (const LayerFrameState &layer : layers) {
        currentLayerIds.push_back(layer.layer.id);
    }
    const SceneSnapshotCache::LayerChanges layerChanges =
        sceneSnapshotCache_.reconcileVisibleLayers(currentLayerIds);
    const std::unordered_set<PointCloudLayerId> addedLayerIds(
        layerChanges.added.begin(), layerChanges.added.end());
    for (const LayerFrameState &layer : layers) {
        if (!addedLayerIds.contains(layer.layer.id)) {
            continue;
        }
        pendingFirstFrameLayerIds_.insert(layer.layer.id);
        pendingDisplayReadyLayerIds_.insert(layer.layer.id);
        publishLoadProgress({
            .layerId = layer.layer.id,
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
        input_.hasMovement() || dragMode_ != DragMode::None;
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
    const auto clip = camera_.clipPlanes();
    const Vec3d eye = camera_.position();
    const Vec3d forward = camera_.forward();
    const Vec3d up = camera_.up();
    const Vec3d right = camera_.right();
    const double halfVertical = camera_.orthographicScale() * 0.5;
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
        .orthographicScale = camera_.orthographicScale(),
        .orthographic = orthographic_,
    };
}

std::vector<BlockDraw>
RenderViewportWidget::buildDrawList(const PointFramePlan &plan)
{
    const FrameCamera frame = currentFrameCamera();
    const float aspect = static_cast<float>(frame.outputWidth) /
                         static_cast<float>(frame.outputHeight);

    QMatrix4x4 projection;
    if (frame.orthographic) {
        const float halfVertical =
            static_cast<float>(frame.orthographicScale * 0.5);
        projection.ortho(-halfVertical * aspect,
                         halfVertical * aspect,
                         -halfVertical,
                         halfVertical,
                         static_cast<float>(frame.nearPlane),
                         static_cast<float>(frame.farPlane));
    } else {
        projection.perspective(
            static_cast<float>(NavigationCamera::verticalFieldOfViewDegrees),
            aspect,
            static_cast<float>(frame.nearPlane),
            static_cast<float>(frame.farPlane));
    }
    QMatrix4x4 view;
    view.lookAt(QVector3D(0.0F, 0.0F, 0.0F),
                QVector3D(static_cast<float>(frame.forward.x),
                          static_cast<float>(frame.forward.y),
                          static_cast<float>(frame.forward.z)),
                QVector3D(static_cast<float>(frame.up.x),
                          static_cast<float>(frame.up.y),
                          static_cast<float>(frame.up.z)));
    const QMatrix4x4 viewProjection =
        rhi()->clipSpaceCorrMatrix() * projection * view;
    const Vec3d eye = frame.eye;
    const double pointSizeScale = static_cast<double>(pointSizePixels_) /
                                  static_cast<double>(defaultPointSizePixels);

    std::vector<BlockDraw> draws;
    draws.reserve(plan.blocks.size());
    quint32 idBase = 0;
    for (const PointFrameSelectedBlock &selected : plan.blocks) {
        QRhiBuffer *buffer = uploadScheduler_.bufferFor(selected.key);
        if (!buffer) {
            continue;
        }
        uploadScheduler_.touch(selected.key, true);
        const PointBlockPtr &block = selected.block;

        const Vec3d relative = block->origin - eye;
        QMatrix4x4 model;
        model.translate(static_cast<float>(relative.x),
                        static_cast<float>(relative.y),
                        static_cast<float>(relative.z));
        model.scale(static_cast<float>(block->scale));
        const QMatrix4x4 mvp = viewProjection * model;

        BlockDraw draw;
        draw.block = block;
        draw.buffer = buffer;
        draw.pointCount = selected.pointCount;
        draw.idBase = idBase;
        draw.uniformIndex = static_cast<quint32>(draws.size());
        std::memcpy(
            draw.uniform.mvp, mvp.constData(), sizeof(draw.uniform.mvp));
        const double pointSpacing =
            selected.pointSpacing > 0.0
                ? selected.pointSpacing
                : std::max(block->bounds.maximumExtent(), 1e-9) /
                      std::sqrt(static_cast<double>(
                          std::max<std::uint32_t>(selected.pointCount, 1U)));
        draw.uniform.pointSize =
            adaptivePointSizePixels(block->bounds,
                                    pointSpacing,
                                    selected.pointCoverageFactor,
                                    pointSizeScale,
                                    static_cast<float>(minimumPointSizePixels),
                                    static_cast<float>(maximumPointSizePixels),
                                    frame);
        draw.uniform.colorSource =
            static_cast<std::int32_t>(selected.colorMode.source);
        draw.uniform.colorMap =
            static_cast<std::int32_t>(selected.colorMode.colorMap);
        std::ranges::copy(selected.classificationFilter.words(),
                          draw.uniform.classificationMask);
        const PointColorMapSampling mapSampling =
            pointColorMapSampling(*colorMaps_, selected.colorMode.colorMap);
        draw.uniform.reserved[0] = mapSampling.rowCoordinate;
        draw.uniform.reserved[1] = mapSampling.inverseWidth;
        draw.uniform.reserved[2] = mapSampling.normalizedSpan;
        double scalarOrigin = 0.0;
        double scalarScale = 1.0;
        const PointScalarRange range =
            selected.colorRange.value_or(PointScalarRange{0.0, 1.0});
        switch (selected.colorMode.source) {
        case PointColorSource::X:
            scalarOrigin = block->origin.x;
            scalarScale = block->scale;
            break;
        case PointColorSource::Y:
            scalarOrigin = block->origin.y;
            scalarScale = block->scale;
            break;
        case PointColorSource::Z:
            scalarOrigin = block->origin.z;
            scalarScale = block->scale;
            break;
        case PointColorSource::Intensity:
        default:
            break;
        }
        const ScalarNormalization normalization = blockScalarNormalization(
            scalarOrigin, scalarScale, range.minimum, range.maximum);
        draw.uniform.scalarOffset = normalization.offset;
        draw.uniform.scalarStep = normalization.step;
        draw.uniform.idBase = static_cast<std::int32_t>(idBase);
        draws.push_back(std::move(draw));
        idBase += selected.pointCount;
    }
    return draws;
}

QMatrix4x4
RenderViewportWidget::frameViewProjection(const FrameCamera &frame) const
{
    const float aspect = static_cast<float>(frame.outputWidth) /
                         static_cast<float>(frame.outputHeight);
    QMatrix4x4 projection;
    if (frame.orthographic) {
        const float halfVertical =
            static_cast<float>(frame.orthographicScale * 0.5);
        projection.ortho(-halfVertical * aspect,
                         halfVertical * aspect,
                         -halfVertical,
                         halfVertical,
                         static_cast<float>(frame.nearPlane),
                         static_cast<float>(frame.farPlane));
    } else {
        projection.perspective(
            static_cast<float>(NavigationCamera::verticalFieldOfViewDegrees),
            aspect,
            static_cast<float>(frame.nearPlane),
            static_cast<float>(frame.farPlane));
    }
    QMatrix4x4 view;
    view.lookAt(QVector3D{},
                QVector3D(static_cast<float>(frame.forward.x),
                          static_cast<float>(frame.forward.y),
                          static_cast<float>(frame.forward.z)),
                QVector3D(static_cast<float>(frame.up.x),
                          static_cast<float>(frame.up.y),
                          static_cast<float>(frame.up.z)));
    return rhi()->clipSpaceCorrMatrix() * projection * view;
}

std::vector<VectorLayerDraw> RenderViewportWidget::buildVectorDrawList() const
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    if (!document) {
        return {};
    }
    const FrameCamera frame = currentFrameCamera();
    const QMatrix4x4 viewProjection = frameViewProjection(frame);
    const Vec3d eye = frame.eye;
    std::vector<VectorLayerDraw> tested;
    std::vector<VectorLayerDraw> alwaysOnTop;
    for (const SceneLayer &sceneLayer : document->layers) {
        const auto *vector = std::get_if<VectorLayerState>(&sceneLayer.payload);
        if (!vector) {
            continue;
        }
        const VectorLayer layer{
            .id = sceneLayer.id,
            .data = vector->data,
            .visible = sceneLayer.visible,
            .style = vector->style,
        };
        if (!layer.visible || !layer.data || layer.data->empty()) {
            continue;
        }
        if (!vectorLayerCullBounds(*layer.data, layer.style, frame)) {
            continue;
        }
        QMatrix4x4 model;
        model.translate(static_cast<float>(layer.data->origin.x - eye.x),
                        static_cast<float>(layer.data->origin.y - eye.y),
                        static_cast<float>(layer.data->origin.z - eye.z +
                                           layer.style.zOffset));
        VectorLayerDraw draw;
        draw.layerId = layer.id;
        draw.layer = layer;
        const QMatrix4x4 mvp = viewProjection * model;
        std::memcpy(
            draw.uniform.mvp.data(), mvp.constData(), sizeof(draw.uniform.mvp));
        draw.uniform.fillColor = {layer.style.fill.red,
                                  layer.style.fill.green,
                                  layer.style.fill.blue,
                                  layer.style.fill.alpha};
        draw.uniform.strokeColor = {layer.style.stroke.red,
                                    layer.style.stroke.green,
                                    layer.style.stroke.blue,
                                    layer.style.stroke.alpha};
        draw.uniform.markerColor = {layer.style.marker.red,
                                    layer.style.marker.green,
                                    layer.style.marker.blue,
                                    layer.style.marker.alpha};
        draw.uniform.viewportPixels = {static_cast<float>(frame.outputWidth),
                                       static_cast<float>(frame.outputHeight)};
        draw.uniform.strokeHalfWidthPixels =
            layer.style.strokeWidthPixels * 0.5F;
        draw.uniform.markerHalfSizePixels = layer.style.markerSizePixels * 0.5F;
        draw.uniform.opacity = layer.style.opacity;
        draw.uniform.markerShape =
            static_cast<std::int32_t>(layer.style.markerShape);
        draw.uniform.featherPixels = 1.0F;
        draw.uniform.nearPlaneW = frame.shaderNearPlaneW();
        (layer.style.alwaysOnTop ? alwaysOnTop : tested)
            .push_back(std::move(draw));
    }
    std::vector<VectorLayerDraw> result;
    result.reserve(tested.size() + alwaysOnTop.size());
    for (VectorLayerDraw &draw : tested) {
        draw.uniformIndex = static_cast<std::uint32_t>(result.size());
        result.push_back(std::move(draw));
    }
    for (VectorLayerDraw &draw : alwaysOnTop) {
        draw.uniformIndex = static_cast<std::uint32_t>(result.size());
        result.push_back(std::move(draw));
    }
    return result;
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
    const auto clip = camera_.clipPlanes();
    const double largestPointSize = largestDrawPointSizePixels(draws);
    const ScreenPickVolume volume =
        makeScreenPickVolume(camera_.position(),
                             camera_.forward(),
                             camera_.right(),
                             camera_.up(),
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
    QRhiCommandBuffer *commandBuffer, const std::vector<BlockDraw> &draws)
{
    if (pointPicker_.inFlight()) {
        return;
    }
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    const std::uint64_t documentRevision = document ? document->revision : 0;
    const auto request = input_.takePendingPick(
        camera_.revision(), documentRevision, selectionGeneration_);
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
    telemetry_.setPicking(picking);
    const bool measurementPick = request->kind == PickKind::MeasureHover ||
                                 request->kind == PickKind::MeasureCommit;
    const int readbackRadius =
        measurementPick
            ? std::clamp(
                  static_cast<int>(std::ceil(10.0 * std::max(scaleX, scaleY))),
                  1,
                  PointPicker::pickTargetEdge / 2 - 1)
            : 2;
    pointPicker_.record(commandBuffer,
                        pointCloudRenderer_.shaderBindings(),
                        candidates,
                        pointCloudRenderer_.uniformStride(),
                        pixelPosition,
                        targetSize,
                        readbackRadius,
                        [this, request = *request, candidates](
                            const std::optional<std::uint32_t> pointId) {
                            resolvePick(request, pointId, candidates);
                        });
}

void RenderViewportWidget::resolvePick(
    const PickRequest request,
    const std::optional<std::uint32_t> pointId,
    const std::vector<BlockDraw> &draws)
{
    if (!input_.isCurrentPick(request)) {
        return;
    }
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    const std::uint64_t documentRevision = document ? document->revision : 0;
    if (request.kind == PickKind::MeasureCommit) {
        input_.completeInFlight();
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
        input_.completeInFlight();
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
    if (request.cameraRevision != camera_.revision() ||
        request.documentRevision != documentRevision ||
        request.selectionGeneration != selectionGeneration_) {
        input_.markStale(request);
        requestRender();
        return;
    }
    input_.completeInFlight();

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
            camera_.setPivot(*target);
        }
    } else if (target) {
        camera_.dollyToward(*target, request.wheelUnits);
    } else {
        camera_.dollyForward(request.wheelUnits);
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
        !document->hasPointCloudLayers() || pointPicker_.inFlight()) {
        return;
    }
    const std::uint64_t serial = input_.queueMeasureHover(position);
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
        camera_,
        QPoint(state.hoverPosition.x, state.hoverPosition.y),
        state.hover,
        state.anchor,
        state.measurement);
}

MeasurementRevisions RenderViewportWidget::measurementRevisions() const noexcept
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    return {
        camera_.revision(),
        document ? document->revision : 0,
        selectionGeneration_,
    };
}

void RenderViewportWidget::updateKeyboardNavigation()
{
    const double deltaSeconds = NavigationInputState::boundedDeltaSeconds(
        static_cast<double>(navigationTimer_.restart()) / 1000.0);
    const Vec3d inputDirection = input_.movementDirection();
    if (length(inputDirection) == 0.0 || deltaSeconds == 0.0) {
        return;
    }

    const Vec3d worldDirection =
        normalized(camera_.right() * inputDirection.x +
                   NavigationCamera::worldUp * inputDirection.y +
                   camera_.forward() * inputDirection.z);
    camera_.translate(worldDirection *
                      camera_.movementSpeed(input_.speedMultiplier()) *
                      deltaSeconds);
}

void RenderViewportWidget::publishMetrics(const bool force)
{
    std::optional<RenderTelemetrySnapshot> telemetry = telemetry_.takeSnapshot(
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
            for (const SceneLayer &layer : document->layers) {
                const auto *point =
                    std::get_if<PointCloudLayerState>(&layer.payload);
                if (!point) {
                    continue;
                }
                decodedPointBytes = saturatingAdd(
                    decodedPointBytes, point->scene->decodedResidentBytes());
                if (layer.visible) {
                    saturatingAccumulate(decodedResidentPoints,
                                         point->scene->decodedResidentPoints());
                }
            }
        }
        const ProcessMemoryMetrics memory = processMemoryMetrics();
        const RasterStreamerMetrics rasterStreamerMetrics =
            rasterTileStreamer_.metrics();
        telemetry->backend = {
            .deviceName = deviceName_,
            .requestedBackend = QString::fromUtf8(
                graphicsApiName(requestedGraphicsApi_).data(),
                static_cast<qsizetype>(
                    graphicsApiName(requestedGraphicsApi_).size())),
            .selectedBackend = backendApiName(api()),
            .timingSource = timingSource_,
            .gpuValidationEnabled = gpuValidationEnabled_,
        };
        telemetry->upload.uniformDrawCapacity = static_cast<std::uint64_t>(
            pointCloudRenderer_.uniformDrawCapacity());
        telemetry->upload.uniformCapacityGrowthCount =
            pointCloudRenderer_.uniformCapacityGrowthCount();
        telemetry->residency = {
            .gpuVectorBytes = vectorLayerRenderer_.gpuBytes(),
            .rasterCpuBytes = rasterStreamerMetrics.cpuBytes,
            .rasterCpuBudgetBytes = rasterTileStreamer_.cpuByteBudget(),
            .rasterCpuPeakBytes = rasterStreamerMetrics.cpuPeakBytes,
            .rasterGpuBytes = rasterLayerRenderer_.gpuBytes(),
            .rasterHeightGpuBytes = rasterLayerRenderer_.heightGpuBytes(),
            .rasterGpuBudgetBytes = rasterLayerRenderer_.gpuByteBudget(),
            .rasterGpuPeakBytes = rasterLayerRenderer_.peakGpuBytes(),
            .rasterTilesRequested = rasterStreamerMetrics.requested,
            .rasterTilesCompleted = rasterStreamerMetrics.completed,
            .rasterTilesCancelled = rasterStreamerMetrics.cancelled,
            .rasterTilesFailed = rasterStreamerMetrics.failed,
            .rasterCacheEvictions = rasterStreamerMetrics.cacheEvictions,
            .rasterUploadedTiles = rasterUploadedTiles_,
            .rasterResidentTiles = rasterLayerRenderer_.residentTileCount(),
            .rasterSelectedTiles = rasterSelectedTiles_,
            .rasterDrawnTiles = rasterDrawnTiles_,
            .rasterSurfaceDrawnTiles = rasterSurfaceDrawnTiles_,
            .rasterSurfaceTriangles =
                static_cast<std::uint64_t>(rasterSurfaceDrawnTiles_) *
                rasterSurfaceGridCellsPerSide *
                rasterSurfaceGridCellsPerSide * 2ULL,
            .rasterPendingReads = rasterStreamerMetrics.pending,
            .rasterFinestLevel = rasterFinestLevel_,
            .rasterCoarsestLevel = rasterCoarsestLevel_,
            .rasterCoverageIncomplete = rasterCoverageIncomplete_,
            .gpuResidentPoints = uploadScheduler_.residentPointCount(),
            .gpuPointBudgetBytes = uploadScheduler_.residencyByteBudget(),
            .gpuPointBytes = uploadScheduler_.residentBytes(),
            .peakGpuPointBytes = uploadScheduler_.peakResidentBytes(),
            .gpuCacheEvictions = uploadScheduler_.evictionCount(),
            .processResidentBytes = memory.residentBytes,
            .peakProcessResidentBytes = memory.peakResidentBytes,
        };
        telemetry->decode = {
            .decodedResidentPoints = decodedResidentPoints,
            .decodedPointBytes = decodedPointBytes,
        };
        const RenderMetrics metrics = projectRenderMetrics(*telemetry);
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
    pointPicker_.releaseResources();
    vectorLayerRenderer_.releaseResources();
    rasterLayerRenderer_.releaseResources();
    rasterDrawnTiles_ = 0;
    rasterSurfaceDrawnTiles_ = 0;
    rasterSurfaceCapability_ = RasterSurfaceCapability::Unknown;
    rasterSurfaceCapabilityReason_.clear();
    if (rasterSurfaceCapabilityCallback_) {
        rasterSurfaceCapabilityCallback_(rasterSurfaceCapability_, {});
    }
    pointCloudRenderer_.releaseResources();
    eyeDomeLightingPass_.releaseResources();
    uploadScheduler_.releaseResources();
    resourceRhi_ = nullptr;
}

void RenderViewportWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (activeTool_ == ViewportTool::Measure) {
            leftPressPosition_ = event->position().toPoint();
            previousMousePosition_ = leftPressPosition_;
            pendingMeasureClick_ = true;
            event->accept();
            return;
        }
        dragMode_ = DragMode::Orbit;
        previousMousePosition_ = event->position().toPoint();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        dragMode_ = DragMode::Pan;
        previousMousePosition_ = event->position().toPoint();
        event->accept();
        return;
    }
    QRhiWidget::mousePressEvent(event);
}

void RenderViewportWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (activeTool_ == ViewportTool::Measure) {
            suppressMeasureRelease_ = true;
            pendingMeasureClick_ = false;
            event->accept();
            return;
        }
        dragMode_ = DragMode::None;
        const QPoint position = event->position().toPoint();
        input_.queuePivot({position.x(), position.y()});
        requestRender();
        event->accept();
        return;
    }
    QRhiWidget::mouseDoubleClickEvent(event);
}

void RenderViewportWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (pendingMeasureClick_) {
        const QPoint position = event->position().toPoint();
        if ((position - leftPressPosition_).manhattanLength() >=
            QApplication::startDragDistance()) {
            pendingMeasureClick_ = false;
            dragMode_ = DragMode::Orbit;
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
                                static_cast<float>(height()));
        }
        requestRender();
        event->accept();
        return;
    }
    if (activeTool_ == ViewportTool::Measure) {
        scheduleMeasurementHover(event->position().toPoint());
        event->accept();
        return;
    }
    QRhiWidget::mouseMoveEvent(event);
}

void RenderViewportWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton &&
        activeTool_ == ViewportTool::Measure) {
        if (suppressMeasureRelease_) {
            suppressMeasureRelease_ = false;
            pendingMeasureClick_ = false;
            event->accept();
            return;
        }
        if (pendingMeasureClick_) {
            pendingMeasureClick_ = false;
            const QPoint position = event->position().toPoint();
            measurementController_.setHoverPosition(
                {position.x(), position.y()});
            static_cast<void>(
                input_.queueMeasureCommit({position.x(), position.y()}));
            requestRender();
            event->accept();
            return;
        }
    }
    const bool releasesOrbit =
        event->button() == Qt::LeftButton && dragMode_ == DragMode::Orbit;
    const bool releasesPan =
        event->button() == Qt::RightButton && dragMode_ == DragMode::Pan;
    if (releasesOrbit || releasesPan) {
        dragMode_ = DragMode::None;
        requestRender();
        event->accept();
        return;
    }
    QRhiWidget::mouseReleaseEvent(event);
}

void RenderViewportWidget::wheelEvent(QWheelEvent *event)
{
    double wheelUnits = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (wheelUnits == 0.0) {
        wheelUnits = static_cast<double>(event->pixelDelta().y()) / 120.0;
    }
    const QPoint position = event->position().toPoint();
    input_.queueWheel({position.x(), position.y()}, wheelUnits);
    requestRender();
    event->accept();
}

namespace {

std::optional<MovementKey> movementKey(const int key)
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
        return MovementKey::Up;
    default:
        return std::nullopt;
    }
}

} // namespace

void RenderViewportWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    if (const auto key = movementKey(event->key())) {
        input_.press(*key);
        navigationTimer_.restart();
        requestRender();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Shift) {
        input_.setFast(true);
        if (input_.hasMovement()) {
            requestRender();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Alt) {
        input_.setFine(true);
        if (input_.hasMovement()) {
            requestRender();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F) {
        camera_.frameScene();
        requestRender();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape &&
        activeTool_ == ViewportTool::Measure) {
        input_.cancelPicks();
        clearMeasurement();
        event->accept();
        return;
    }
    QRhiWidget::keyPressEvent(event);
}

void RenderViewportWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    if (const auto key = movementKey(event->key())) {
        input_.release(*key);
        requestRender();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Shift) {
        input_.setFast(false);
        if (input_.hasMovement()) {
            requestRender();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Alt) {
        input_.setFine(false);
        if (input_.hasMovement()) {
            requestRender();
        }
        event->accept();
        return;
    }
    QRhiWidget::keyReleaseEvent(event);
}

void RenderViewportWidget::focusOutEvent(QFocusEvent *event)
{
    input_.clearMovement();
    dragMode_ = DragMode::None;
    pendingMeasureClick_ = false;
    measurementController_.clearHover();
    updateMeasurementOverlay();
    requestRender();
    QRhiWidget::focusOutEvent(event);
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
