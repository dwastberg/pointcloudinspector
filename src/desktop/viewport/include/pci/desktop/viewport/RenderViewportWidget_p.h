#pragma once

#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/navigation/NavigationCamera.h>
#include <pci/navigation/NavigationInputState.h>
#include <pci/navigation/QualificationCameraPath.h>
#include <pci/rendering/FrameExecutor.h>
#include <pci/rendering/SceneSnapshotCache.h>
#include <pci/rendering/planning/AdaptivePointBudget.h>
#include <pci/rendering/planning/MeasurementController.h>
#include <pci/rendering/planning/PointFrameCoordinator.h>
#include <pci/rendering/planning/RasterFrameCoordinator.h>
#include <pci/rendering/planning/RenderActivity.h>
#include <pci/rendering/rhi/FrameRenderer.h>
#include <pci/rendering/rhi/PointCloudRenderer.h>
#include <pci/rendering/rhi/PointPicker.h>
#include <pci/rendering/rhi/RasterLayerRenderer.h>
#include <pci/rendering/rhi/UploadScheduler.h>
#include <pci/rendering/rhi/VectorLayerRenderer.h>
#include <pci/runtime/CompletionExecutor.h>
#include <pci/runtime/raster/RasterTileStreamer.h>

#include <pci/rendering/RenderTelemetry.h>

#include <QElapsedTimer>
#include <QPoint>
#include <QPointer>
#include <QRhiWidget>
#include <QSize>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class QMouseEvent;
class QFocusEvent;
class QKeyEvent;
class QResizeEvent;
class QRhi;
class QRhiCommandBuffer;
class QTimer;
class QWheelEvent;

namespace pci {

class MeasurementOverlay;
class RenderViewportTestAccess;
class ViewportInputController;

class RenderViewportWidget final
    : public QRhiWidget
    , public RenderViewport {
public:
    RenderViewportWidget(bool smokeTest,
                         std::uint64_t gpuByteBudget =
                             UploadScheduler::defaultResidencyByteBudget,
                         GraphicsApi graphicsApi = GraphicsApi::Auto,
                         bool enableGpuValidation = false,
                         PointColorMapCatalogSnapshotPtr colorMaps = {});
    ~RenderViewportWidget() override;

    [[nodiscard]] QWidget *widget() noexcept override;
    [[nodiscard]] QString backendName() const override;
    [[nodiscard]] std::uint64_t totalPointCount() const noexcept override;
    using RenderViewport::setDocument;
    void setDocument(SceneDocumentSnapshotPtr document,
                     SceneRuntimeSnapshotPtr runtime,
                     RuntimeBudgetSnapshot runtimeBudget,
                     SessionGeneration sessionGeneration,
                     bool frameVisibleLayers) override;
    using RenderViewport::updateDocument;
    void updateDocument(SceneDocumentSnapshotPtr document,
                        SceneRuntimeSnapshotPtr runtime,
                        RuntimeBudgetSnapshot runtimeBudget,
                        SessionGeneration sessionGeneration) override;
    void requestRender() override;
    void frameVisibleLayers() override;
    void frameVisibleLayersTopDown() override;
    [[nodiscard]] bool isOrthographic() const noexcept override;
    void setOrthographic(bool enabled) override;
    [[nodiscard]] bool isMapView() const noexcept override;
    void setMapView(bool enabled) override;
    void frameLayer(PointCloudLayerId layerId) override;
    [[nodiscard]] bool eyeDomeLightingEnabled() const noexcept override;
    void setEyeDomeLightingEnabled(bool enabled) override;
    [[nodiscard]] ViewportSettings viewportSettings() const noexcept override;
    void setViewportSettings(const ViewportSettings &settings) override;
    [[nodiscard]] std::uint64_t gpuByteBudget() const noexcept override;
    void setGpuByteBudget(std::uint64_t byteBudget) override;
    void setRasterByteBudgets(std::uint64_t cpuByteBudget,
                              std::uint64_t gpuByteBudget) override;
    [[nodiscard]] int pointSizePixels() const noexcept override;
    void setPointSizePixels(int pointSize) override;
    [[nodiscard]] ViewportTool activeTool() const noexcept override;
    void setActiveTool(ViewportTool tool) override;
    void setMetricsCallback(MetricsCallback callback) override;
    void setContinuousMetricsEnabled(bool enabled) override;
    [[nodiscard]] bool startQualificationCameraPath() override;
    void setFailureCallback(FailureCallback callback) override;
    void setLoadProgressCallback(LoadProgressCallback callback) override;
    [[nodiscard]] VectorOverlayCapability
    vectorOverlayCapability() const noexcept override;
    void setVectorOverlayCapabilityCallback(
        VectorOverlayCapabilityCallback callback) override;
    [[nodiscard]] RasterSurfaceCapability
    rasterSurfaceCapability() const noexcept override;
    void setRasterSurfaceCapabilityCallback(
        RasterSurfaceCapabilityCallback callback) override;

protected:
    void initialize(QRhiCommandBuffer *commandBuffer) override;
    void render(QRhiCommandBuffer *commandBuffer) override;
    void releaseResources() override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    friend class RenderViewportTestAccess;
    friend class ViewportInputController;

    using LayerFrameState = PointFrameLayer;

    void refreshDocumentState(const std::vector<LayerFrameState> &layers);
    void queueSceneInvalidation();
    void applyQualificationCameraFrame();
    void frameBounds(const std::optional<Bounds3d> &bounds,
                     double distanceMultiplier = 2.0);
    [[nodiscard]] std::uint64_t
    framePointBudget(const std::vector<LayerFrameState> &layers) const noexcept;
    [[nodiscard]] FrameCamera currentFrameCamera() const;
    struct RasterFrameCapture {
        std::vector<RasterLayerSnapshot> layers;
        std::vector<RasterFrameLayerInput> inputs;
        RasterFrameInput input;
    };
    [[nodiscard]] RasterFrameCapture captureRasterFrame();
    void updateSelectionGeneration(const std::vector<BlockDraw> &draws);
    [[nodiscard]] std::vector<BlockDraw>
    pickCandidates(const std::vector<BlockDraw> &draws,
                   PixelPosition position,
                   QSize targetSize) const;
    void updateKeyboardNavigation();
    void applyKeyboardNavigation(double deltaSeconds);
    void submitPendingPick(QRhiCommandBuffer *commandBuffer,
                           const std::vector<BlockDraw> &draws,
                           const std::vector<PointCloudNodePayloadPtr> &leases);
    void resolvePick(PickRequest request,
                     std::optional<std::uint32_t> pointId,
                     const std::vector<BlockDraw> &draws);
    [[nodiscard]] std::optional<Vec3d>
    resolvePickedPoint(std::optional<std::uint32_t> pointId,
                       const std::vector<BlockDraw> &draws) const;
    void queueMeasurementHover(PixelPosition position);
    void scheduleMeasurementHover(QPoint position);
    void clearMeasurement();
    void updateMeasurementOverlay();
    [[nodiscard]] MeasurementRevisions measurementRevisions() const noexcept;
    [[nodiscard]] std::chrono::duration<double, std::milli>
    frameDuration(QRhiCommandBuffer *commandBuffer);
    void publishMetrics(bool force);
    void publishLoadProgress(RenderLoadProgress progress);
    void fail(const QString &message);
    [[nodiscard]] bool ensureRenderResources();

    std::uint64_t pointCount_ = 0;
    bool smokeTest_;
    GraphicsApi requestedGraphicsApi_ = GraphicsApi::Auto;
    bool gpuValidationEnabled_ = false;
#ifdef PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
    bool profileRendering_ = false;
    // PCI_PROBE_RESIDENCY traces why a layer has not reached display readiness.
    // A batch load only retires a job once every layer publishes DisplayReady,
    // so a permanently blocked layer freezes the whole load with no other
    // diagnostic surface.
    bool probeResidency_ = false;
    std::chrono::steady_clock::time_point nextResidencyProbe_;
    std::uint64_t probeDroppedUploads_ = 0;
    std::uint64_t probeDroppedBytes_ = 0;
#endif
    SceneSnapshotCache sceneSnapshotCache_;
    SceneRuntimeSnapshotPtr runtimeSnapshot_;
    RuntimeBudgetSnapshot runtimeBudget_;
    std::unordered_set<PointCloudLayerId> pendingFirstFrameLayerIds_;
    std::unordered_set<PointCloudLayerId> pendingDisplayReadyLayerIds_;
    // Carried out of the frame plan so the display-readiness pass, which runs
    // after the plan goes out of scope, can tell "no residency yet" apart from
    // "off screen, so never any residency".
    std::unordered_set<PointCloudLayerId> outOfFrustumLayerIds_;
    std::unordered_map<PointCloudLayerId, std::uint64_t>
        publishedResidentPointCounts_;
    std::unordered_map<PointCloudLayerId, std::string> hierarchyLayerErrors_;
    SessionGeneration sessionGeneration_{1};
    std::shared_ptr<CompletionExecutor> completionExecutor_;
    FrameExecutor frameExecutor_;
    std::unique_ptr<ViewportInputController> inputController_;
    std::optional<std::uint64_t> pendingPointGpuBudget_;
    std::optional<std::pair<std::uint64_t, std::uint64_t>>
        pendingRasterBudgets_;
    MetricsCallback metricsCallback_;
    FailureCallback failureCallback_;
    LoadProgressCallback loadProgressCallback_;
    VectorOverlayCapabilityCallback vectorOverlayCapabilityCallback_;
    VectorOverlayCapability vectorOverlayCapability_ =
        VectorOverlayCapability::Unknown;
    RasterSurfaceCapabilityCallback rasterSurfaceCapabilityCallback_;
    RasterSurfaceCapability rasterSurfaceCapability_ =
        RasterSurfaceCapability::Unknown;
    QString rasterSurfaceCapabilityReason_;

    QRhi *resourceRhi_ = nullptr;

    AdaptivePointBudget pointBudget_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    FrameRenderer renderer_;
    struct RasterDecodeState {
        std::uint64_t generation = 0;
        std::shared_ptr<const RasterDecodeParameters> parameters;
    };
    std::unordered_map<SceneLayerId, RasterDecodeState> rasterDecodeStates_;
    std::size_t rasterSelectedTiles_ = 0;
    std::size_t rasterDrawnTiles_ = 0;
    std::size_t rasterSurfaceDrawnTiles_ = 0;
    std::uint64_t rasterUploadedTiles_ = 0;
    std::uint32_t rasterFinestLevel_ = 0;
    std::uint32_t rasterCoarsestLevel_ = 0;
    bool rasterCoverageIncomplete_ = false;
    std::vector<GpuBlockKey> currentGpuProtection_;
    std::vector<GpuBlockKey> pickGpuProtection_;
    std::shared_ptr<bool> pickDelivery_ = std::make_shared<bool>(true);
    MeasurementOverlay *measurementOverlay_ = nullptr;
    PointPicker::Completion rawPickCompletion_;
    ViewportTool activeTool_ = ViewportTool::Navigate;
    MeasurementController measurementController_;
    QTimer *measurementHoverTimer_ = nullptr;
    QString deviceName_;
    QString timingSource_ = QStringLiteral("CPU");
    bool failed_ = false;
    ViewportSettings viewportSettings_;
    bool orthographic_ = false;
    bool mapView_ = false;
    bool orthographicBeforeMapView_ = false;
    bool eyeDomeLightingActive_ = false;
    int pointSizePixels_ = defaultPointSizePixels;
    struct DrawSignature {
        const PointBlock *block = nullptr;
        quint32 pointCount = 0;
        quint32 idBase = 0;

        bool operator==(const DrawSignature &) const = default;
    };
    std::vector<DrawSignature> drawSignature_;
    std::uint64_t selectionGeneration_ = 0;
    bool sceneInvalidationPending_ = false;
    bool frameTimingContinuous_ = false;
    bool continuousMetricsEnabled_ = false;
    std::optional<QualificationCameraPath> qualificationCameraPath_;
    std::chrono::steady_clock::time_point previousCpuFrame_;
    QElapsedTimer navigationTimer_;
};

} // namespace pci
