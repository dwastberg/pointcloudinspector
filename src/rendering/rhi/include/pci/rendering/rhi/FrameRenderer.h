#pragma once

#include <pci/rendering/planning/RasterFrameCoordinator.h>
#include <pci/rendering/planning/RenderSettings.h>
#include <pci/rendering/rhi/EyeDomeLightingPass.h>
#include <pci/rendering/rhi/PointPicker.h>
#include <pci/rendering/rhi/RasterLayerRenderer.h>
#include <pci/rendering/rhi/UploadScheduler.h>
#include <pci/rendering/rhi/VectorLayerRenderer.h>
#include <pci/runtime/raster/RasterTileStreamer.h>

#include <QMatrix4x4>

namespace pci {

// QRhi implementation of frame submission. Desktop owns the widget/context;
// this component owns GPU resources and converts portable plans into draws.
class FrameRenderer final {
public:
    FrameRenderer(std::uint64_t gpuBudget,
                  PointColorMapCatalogSnapshotPtr colorMaps)
        : uploads(gpuBudget)
        , points(colorMaps)
        , colorMaps_(std::move(colorMaps))
    {
    }

    struct RasterSubmission {
        std::vector<RasterLayerDraw> draws;
        std::size_t uploaded = 0;
        std::size_t surfaceDrawn = 0;
        bool requiresContinuation = false;
    };
    [[nodiscard]] std::vector<BlockDraw> pointDraws(QRhi *rhi,
                                                    const FrameCamera &frame,
                                                    const PointFramePlan &plan,
                                                    int pointSizePixels);
    [[nodiscard]] std::vector<VectorLayerDraw>
    vectorDraws(QRhi *rhi,
                const FrameCamera &frame,
                const SceneDocumentSnapshotPtr &document) const;
    [[nodiscard]] RasterSubmission
    rasterDraws(QRhi *rhi,
                QRhiCommandBuffer *commandBuffer,
                const FrameCamera &frame,
                std::span<const RasterLayerSnapshot> layers,
                const RasterFramePlan &plan,
                RasterTileStreamer &streamer);
    void record(QRhiCommandBuffer *commandBuffer,
                QRhiRenderTarget *output,
                const ViewportSettings &settings,
                bool edlActive,
                const std::vector<BlockDraw> &draws,
                std::span<const VectorLayerDraw> vectorDraws,
                std::span<const RasterLayerDraw> rasterDraws);
    void releaseResources();

    UploadScheduler uploads;
    PointCloudRenderer points;
    VectorLayerRenderer vectors;
    RasterLayerRenderer rasters;
    EyeDomeLightingPass edl;
    PointPicker picker;

private:
    friend class RenderViewportTestAccess;
    [[nodiscard]] static QMatrix4x4 viewProjection(QRhi *rhi,
                                                   const FrameCamera &frame);
    PointColorMapCatalogSnapshotPtr colorMaps_;
    std::vector<RasterSourceId> knownRasterSources_;
};

} // namespace pci
