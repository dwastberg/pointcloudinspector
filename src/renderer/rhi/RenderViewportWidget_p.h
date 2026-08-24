#pragma once

#include "navigation/NavigationCamera.h"
#include "navigation/NavigationInputState.h"
#include "renderer/RenderViewport.h"
#include "renderer/planning/AdaptivePointBudget.h"
#include "renderer/planning/MeasurementController.h"
#include "renderer/planning/PointFrameCoordinator.h"
#include "renderer/planning/RenderActivity.h"
#include "renderer/rhi/EyeDomeLightingPass.h"
#include "renderer/rhi/PointCloudRenderer.h"
#include "renderer/rhi/PointPicker.h"
#include "renderer/rhi/RasterLayerRenderer.h"
#include "renderer/rhi/RasterTileStreamer.h"
#include "renderer/rhi/SceneSnapshotCache.h"
#include "renderer/rhi/UploadScheduler.h"
#include "renderer/rhi/VectorLayerRenderer.h"
#include "renderer/telemetry/RenderTelemetryAccumulator.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QPointer>
#include <QRhiWidget>
#include <QSize>

#include <chrono>
#include <cstdint>
#include <optional>
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
    void setDocument(SceneDocumentSnapshotPtr document,
                     bool frameVisibleLayers) override;
    void updateDocument(SceneDocumentSnapshotPtr document) override;
    void requestRender() override;
    void frameVisibleLayers() override;
    void frameVisibleLayersTopDown() override;
    [[nodiscard]] bool isOrthographic() const noexcept override;
    void setOrthographic(bool enabled) override;
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
    void setFailureCallback(FailureCallback callback) override;
    void setLoadProgressCallback(LoadProgressCallback callback) override;
    [[nodiscard]] VectorOverlayCapability
    vectorOverlayCapability() const noexcept override;
    void setVectorOverlayCapabilityCallback(
        VectorOverlayCapabilityCallback callback) override;

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

    enum class DragMode {
        None,
        Orbit,
        Pan,
    };

    using LayerFrameState = PointFrameLayer;

    void refreshDocumentState(const std::vector<LayerFrameState> &layers);
    void refreshFullDetailPlan(const std::vector<LayerFrameState> &layers);
    void publishFullDetailProgress();
    void clearFullDetailPlan();
    void queueSceneInvalidation();
    void frameBounds(const std::optional<Bounds3d> &bounds,
                     double distanceMultiplier = 2.0);
    [[nodiscard]] std::uint64_t
    framePointBudget(const std::vector<LayerFrameState> &layers) const noexcept;
    [[nodiscard]] FrameCamera currentFrameCamera() const;
    [[nodiscard]] std::vector<BlockDraw>
    buildDrawList(const PointFramePlan &plan);
    [[nodiscard]] std::vector<VectorLayerDraw> buildVectorDrawList() const;
    struct RasterFrameResult {
        std::vector<RasterLayerDraw> draws;
        bool requiresContinuation = false;
    };
    [[nodiscard]] RasterFrameResult
    streamRasterTiles(QRhiCommandBuffer *commandBuffer);
    [[nodiscard]] QMatrix4x4
    frameViewProjection(const FrameCamera &frame) const;
    void updateSelectionGeneration(const std::vector<BlockDraw> &draws);
    void recordScene(QRhiCommandBuffer *commandBuffer,
                     const std::vector<BlockDraw> &draws,
                     std::span<const VectorLayerDraw> vectorDraws,
                     std::span<const RasterLayerDraw> rasterDraws);
    [[nodiscard]] std::vector<BlockDraw>
    pickCandidates(const std::vector<BlockDraw> &draws,
                   PixelPosition position,
                   QSize targetSize) const;
    void updateKeyboardNavigation();
    void submitPendingPick(QRhiCommandBuffer *commandBuffer,
                           const std::vector<BlockDraw> &draws);
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
    std::unordered_set<PointCloudLayerId> pendingFirstFrameLayerIds_;
    std::unordered_set<PointCloudLayerId> pendingDisplayReadyLayerIds_;
    // Carried out of the frame plan so the display-readiness pass, which runs
    // after the plan goes out of scope, can tell "no residency yet" apart from
    // "off screen, so never any residency".
    std::unordered_set<PointCloudLayerId> outOfFrustumLayerIds_;
    std::unordered_map<PointCloudLayerId, std::uint64_t>
        publishedResidentPointCounts_;
    PointFrameCoordinator pointFrameCoordinator_;
    MetricsCallback metricsCallback_;
    FailureCallback failureCallback_;
    LoadProgressCallback loadProgressCallback_;
    VectorOverlayCapabilityCallback vectorOverlayCapabilityCallback_;
    VectorOverlayCapability vectorOverlayCapability_ =
        VectorOverlayCapability::Unknown;

    QRhi *resourceRhi_ = nullptr;

    AdaptivePointBudget pointBudget_;
    UploadScheduler uploadScheduler_;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    PointCloudRenderer pointCloudRenderer_;
    VectorLayerRenderer vectorLayerRenderer_;
    RasterLayerRenderer rasterLayerRenderer_;
    RasterTileStreamer rasterTileStreamer_{rasterDefaultCpuCacheBytes};
    struct RasterDecodeState {
        std::uint64_t generation = 0;
        std::shared_ptr<const RasterDecodeParameters> parameters;
    };
    std::unordered_map<SceneLayerId, RasterDecodeState> rasterDecodeStates_;
    std::unordered_map<SceneLayerId, std::vector<RasterTileKey>>
        previousRasterSelection_;
    // Sources the last frame saw, so a source that disappears can be released
    // rather than left holding decoded tiles and queued reads.
    std::vector<RasterSourceId> knownRasterSources_;
    std::size_t rasterSelectedTiles_ = 0;
    std::size_t rasterDrawnTiles_ = 0;
    std::uint64_t rasterUploadedTiles_ = 0;
    std::uint32_t rasterFinestLevel_ = 0;
    std::uint32_t rasterCoarsestLevel_ = 0;
    bool rasterCoverageIncomplete_ = false;
    EyeDomeLightingPass eyeDomeLightingPass_;
    PointPicker pointPicker_;
    MeasurementOverlay *measurementOverlay_ = nullptr;
    NavigationCamera camera_;
    NavigationInputState input_;
    PointPicker::Completion rawPickCompletion_;
    DragMode dragMode_ = DragMode::None;
    ViewportTool activeTool_ = ViewportTool::Navigate;
    QPoint previousMousePosition_;
    QPoint leftPressPosition_;
    bool pendingMeasureClick_ = false;
    bool suppressMeasureRelease_ = false;
    MeasurementController measurementController_;
    QTimer *measurementHoverTimer_ = nullptr;
    QString deviceName_;
    QString timingSource_ = QStringLiteral("CPU");
    bool failed_ = false;
    ViewportSettings viewportSettings_;
    bool orthographic_ = false;
    bool eyeDomeLightingActive_ = false;
    int pointSizePixels_ = defaultPointSizePixels;
    RenderTelemetryAccumulator telemetry_;
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
    std::chrono::steady_clock::time_point previousCpuFrame_;
    QElapsedTimer navigationTimer_;
};

} // namespace pci
