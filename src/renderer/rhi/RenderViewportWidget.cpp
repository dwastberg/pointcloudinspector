#include "renderer/rhi/RenderViewportWidget_p.h"

#include "foundation/CheckedArithmetic.h"
#include "platform/ProcessMemory.h"
#include "renderer/PointColorMapAtlas.h"
#include "renderer/planning/FrustumCuller.h"
#include "renderer/rhi/BackendPolicy.h"

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
    clearFullDetailPlan();
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
    clearFullDetailPlan();
    sceneSnapshotCache_.setDocument(std::move(document), true);
    pendingFirstFrameLayerIds_.clear();
    pendingDisplayReadyLayerIds_.clear();
    publishedResidentPointCounts_.clear();
    static_cast<void>(pointFrameCoordinator_.clear());
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

std::vector<RasterLayerDraw> RenderViewportWidget::buildRasterDrawList() const
{
    const SceneDocumentSnapshotPtr &document = sceneSnapshotCache_.document();
    if (!document || document->rasterLayerCount() == 0) {
        return {};
    }
    const FrameCamera frame = currentFrameCamera();
    const QMatrix4x4 viewProjection = frameViewProjection(frame);

    std::vector<RasterLayerDraw> result;
    result.reserve(document->rasterLayerCount());
    // Document order is painter order: a later raster layer covers an earlier
    // one because raster draws do not write depth.
    for (const SceneLayer &sceneLayer : document->layers) {
        const auto *raster = std::get_if<RasterLayerState>(&sceneLayer.payload);
        if (raster == nullptr || !sceneLayer.visible || !raster->data) {
            continue;
        }
        const RasterLayerMetadata &metadata = raster->data->metadata();
        if (!rasterLayerCullBounds(metadata, raster->style, frame)) {
            continue;
        }

        // Eye-relative in double precision, then narrowed, so placement stays
        // stable at large projected coordinates.
        const RasterQuadTransform quad =
            rasterLayerQuadTransform(metadata, raster->style, frame.eye);
        QMatrix4x4 model;
        model.setColumn(0,
                        QVector4D(static_cast<float>(quad.edgeU.x),
                                  static_cast<float>(quad.edgeU.y),
                                  0.0F,
                                  0.0F));
        model.setColumn(1,
                        QVector4D(static_cast<float>(quad.edgeV.x),
                                  static_cast<float>(quad.edgeV.y),
                                  0.0F,
                                  0.0F));
        model.setColumn(2, QVector4D(0.0F, 0.0F, 1.0F, 0.0F));
        model.setColumn(3,
                        QVector4D(static_cast<float>(quad.origin.x),
                                  static_cast<float>(quad.origin.y),
                                  static_cast<float>(quad.origin.z),
                                  1.0F));

        RasterLayerDraw draw;
        draw.layerId = sceneLayer.id;
        const QMatrix4x4 mvp = viewProjection * model;
        std::memcpy(
            draw.uniform.mvp.data(), mvp.constData(), sizeof(draw.uniform.mvp));
        // Phase 1 uploads the whole image with no gutter, so the inner extent
        // is the full texture.
        draw.uniform.uvRect = {0.0F, 0.0F, 1.0F, 1.0F};
        draw.uniform.opacity = raster->style.opacity;
        result.push_back(draw);
    }
    return result;
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
    if (!pointFrameCoordinator_.fullDetailPlanned() &&
        telemetry_.frameCount() > 0 && duration.count() > 0.0) {
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
            publishFullDetailProgress();

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
            rasterDraws = buildRasterDrawList();
            updateSelectionGeneration(draws);
            pointCloudRenderer_.updateUniforms(commandBuffer, draws);
            vectorLayerRenderer_.syncLayers(commandBuffer, document->layers);
            vectorLayerRenderer_.updateUniforms(commandBuffer, vectorDraws);
            // Texture creation and uploads must complete before beginPass().
            rasterLayerRenderer_.syncLayers(
                commandBuffer, document->layers, colorMaps_);
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
                       "pending_display=%10 full_detail=%11")
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
                       .arg(pendingDisplayReadyLayerIds_.size())
                       .arg(!pointFrameCoordinator_.fullDetailPlanned()
                                ? QStringLiteral("none")
                            : pointFrameCoordinator_.fullDetailActive()
                                ? QStringLiteral("active")
                                : QStringLiteral("warming"));
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
            (uploadScheduler_.residentPointCount(layerId) == 0 && !offScreen) ||
            pointFrameCoordinator_.fullDetailDecisionPending()) {
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
                                    : QStringLiteral(
                                          "full-detail-decision-pending"))
                           .arg(uploadScheduler_.residentPointCount(layerId));
            }
#endif
            ++pending;
            continue;
        }

        std::uint64_t completed = uploadScheduler_.residentPointCount(layerId);
        std::uint64_t total = snapshot->hierarchical
                                  ? snapshot->sourcePointCount
                                  : snapshot->retainedFlatPointCount;
        std::uint64_t decoded = 0;
        std::uint64_t uploadedPoints = 0;
        if (const std::optional<FullDetailLayerStatus> planned =
                pointFrameCoordinator_.fullDetailLayerStatus(layerId)) {
            if (!pointFrameCoordinator_.fullDetailActive()) {
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
                if (probeResidency_ && probeDue) {
                    qInfo().noquote()
                        << QStringLiteral(
                               "[probe] display-blocked layer=%1 "
                               "reason=full-detail-warming decoded=%2/%3")
                               .arg(layerId.value())
                               .arg(planned->decodedNodes)
                               .arg(planned->totalNodes);
                }
#endif
                ++pending;
                continue;
            }
            completed = planned->pointCount;
            total = planned->pointCount;
            decoded = total;
            uploadedPoints = total;
        }
        publishLoadProgress({
            .layerId = layerId,
            .stage = RenderLoadStage::DisplayReady,
            .completed = completed,
            .total = total,
            .decoded = decoded,
            .uploaded = uploadedPoints,
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
        .pendingUploads = uploadsNeedAnotherFrame || planNeedsAnotherFrame,
        .sceneInvalidation = sceneInvalidationPending_,
        .pendingSmokeFrames = smokeTest_ && telemetry_.frameCount() < 3,
    });
    // Event-driven rendering may not submit another frame for a long time.
    // Publish the settling frame even inside the normal sampling interval so
    // diagnostics capture the final selected/submitted counts and frame total.
    publishMetrics(!continueRendering || telemetry_.frameCount() == 1);

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
    refreshFullDetailPlan(layers);

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

void RenderViewportWidget::refreshFullDetailPlan(
    const std::vector<LayerFrameState> &layers)
{
    const FullDetailConfigurationChange change =
        pointFrameCoordinator_.configureFullDetail(
            sceneSnapshotCache_.document(),
            layers,
            uploadScheduler_.residencyByteBudget());
    if (change.planStarted) {
        pointBudget_.setCurrent(pointBudget_.total());
    } else if (change.planStopped) {
        pointBudget_.setCurrent(
            std::min<std::uint64_t>(1'000'000, pointBudget_.total()));
    }
}

void RenderViewportWidget::publishFullDetailProgress()
{
    const auto resident = [this](const PointFrameBlockKey &id) {
        return uploadScheduler_.bufferFor({
                   .layerId = id.layerId,
                   .nodeId = id.nodeId,
                   .nodeBlockIndex = id.nodeBlockIndex,
               }) != nullptr;
    };
    for (const FullDetailProgressUpdate &progress :
         pointFrameCoordinator_.fullDetailProgress(resident)) {
        publishLoadProgress({
            .layerId = progress.layerId,
            .stage = RenderLoadStage::FullDetailWarming,
            .completed = std::min(progress.decoded, progress.uploaded),
            .total = progress.total,
            .decoded = progress.decoded,
            .uploaded = progress.uploaded,
        });
    }
}

void RenderViewportWidget::clearFullDetailPlan()
{
    if (pointFrameCoordinator_.clear().planStopped) {
        pointBudget_.setCurrent(
            std::min<std::uint64_t>(1'000'000, pointBudget_.total()));
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
    if (pointFrameCoordinator_.fullDetailPlanned()) {
        return pointBudget_.total();
    }
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
        draw.uniform.pointSize = static_cast<float>(pointSizePixels_);
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
    const ScreenPickVolume volume = makeScreenPickVolume(
        camera_.position(),
        camera_.forward(),
        camera_.right(),
        camera_.up(),
        NavigationCamera::verticalFieldOfViewDegrees,
        targetSize.width(),
        targetSize.height(),
        static_cast<double>(position.x),
        static_cast<double>(position.y),
        std::max(4.0, static_cast<double>(pointSizePixels_) * 0.5 + 1.0),
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
        const FullDetailStatus fullDetail =
            pointFrameCoordinator_.fullDetailStatus();
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
            .gpuResidentPoints = uploadScheduler_.residentPointCount(),
            .gpuPointBudgetBytes = uploadScheduler_.residencyByteBudget(),
            .gpuPointBytes = uploadScheduler_.residentBytes(),
            .peakGpuPointBytes = uploadScheduler_.peakResidentBytes(),
            .gpuCacheEvictions = uploadScheduler_.evictionCount(),
            .processResidentBytes = memory.residentBytes,
            .peakProcessResidentBytes = memory.peakResidentBytes,
            .fullDetailWarming = fullDetail.warming,
            .fullDetailActive = fullDetail.active,
            .fullDetailDecodedNodes = fullDetail.decodedNodes,
            .fullDetailTotalNodes = fullDetail.totalNodes,
            .fullDetailDecodesInFlight = fullDetail.decodesInFlight,
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
                       "full_detail=%20 nodes=%21/%22 inflight_sources=%23 "
                       "cpu_bytes=%24/%25")
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
                       .arg(metrics.fullDetailActive
                                ? QStringLiteral("resident")
                            : metrics.fullDetailWarming
                                ? QStringLiteral("warming")
                                : QStringLiteral("lod"))
                       .arg(metrics.fullDetailDecodedNodes)
                       .arg(metrics.fullDetailTotalNodes)
                       .arg(metrics.fullDetailDecodesInFlight)
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
