#include <QColor>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <pci/navigation/NavigationCamera.h>
#include <pci/rendering/planning/PointSizePolicy.h>
#include <pci/rendering/rhi/FrameRenderer.h>
#include <pci/rendering/rhi/PointColorMapAtlas.h>
#include <ranges>

namespace pci {

std::vector<BlockDraw> FrameRenderer::pointDraws(QRhi *rhi,
                                                 const FrameCamera &frame,
                                                 const PointFramePlan &plan,
                                                 const int pointSizePixels)
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
    view.lookAt(QVector3D(0.0F, 0.0F, 0.0F),
                QVector3D(static_cast<float>(frame.forward.x),
                          static_cast<float>(frame.forward.y),
                          static_cast<float>(frame.forward.z)),
                QVector3D(static_cast<float>(frame.up.x),
                          static_cast<float>(frame.up.y),
                          static_cast<float>(frame.up.z)));
    const QMatrix4x4 viewProjection =
        rhi->clipSpaceCorrMatrix() * projection * view;
    const Vec3d eye = frame.eye;
    const double pointSizeScale = static_cast<double>(pointSizePixels) /
                                  static_cast<double>(defaultPointSizePixels);

    std::vector<BlockDraw> draws;
    draws.reserve(plan.blocks.size());
    quint32 idBase = 0;
    for (const PointFrameSelectedBlock &selected : plan.blocks) {
        QRhiBuffer *buffer = uploads.bufferFor(selected.key);
        if (!buffer) {
            continue;
        }
        uploads.touch(selected.key, true);
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

QMatrix4x4 FrameRenderer::viewProjection(QRhi *rhi, const FrameCamera &frame)
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
    return rhi->clipSpaceCorrMatrix() * projection * view;
}

std::vector<VectorLayerDraw>
FrameRenderer::vectorDraws(QRhi *rhi,
                           const FrameCamera &frame,
                           const SceneDocumentSnapshotPtr &document) const
{
    if (!document) {
        return {};
    }
    const QMatrix4x4 viewProjection = FrameRenderer::viewProjection(rhi, frame);
    const Vec3d eye = frame.eye;
    std::vector<VectorLayerDraw> tested;
    std::vector<VectorLayerDraw> alwaysOnTop;
    for (const SceneSnapshotLayer &sceneLayer : document->layers) {
        const auto *vector =
            std::get_if<VectorLayerSnapshotState>(&sceneLayer.payload);
        if (!vector) {
            continue;
        }
        const VectorLayer layer{
            .id = sceneLayer.id,
            .data = vector->data,
            .visible = sceneLayer.visible,
            .style = vector->style,
            .bindingGeneration = sceneLayer.bindingGeneration,
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

FrameRenderer::RasterSubmission
FrameRenderer::rasterDraws(QRhi *rhi,
                           QRhiCommandBuffer *commandBuffer,
                           const FrameCamera &frame,
                           const std::span<const RasterLayerSnapshot> layers,
                           const RasterFramePlan &rasterPlan,
                           RasterTileStreamer &streamer)
{
    const QMatrix4x4 viewProjection = FrameRenderer::viewProjection(rhi, frame);
    std::vector<RasterLayerDraw> draws;
    std::vector<RasterPendingUpload> pending;
    for (const RasterFrameLayerPlan &plan : rasterPlan.layers) {
        const RasterLayerSnapshot &layer = layers[plan.inputIndex];
        const RasterSourceId sourceId = layer.descriptor.sourceId;
        const RasterLayerMetadata &metadata = layer.descriptor.metadata;
        const std::uint64_t generation = layer.renderGeneration;
        const RasterTilePayloadProfile profile = plan.profile;
        const bool surface = plan.surface;
        // plan.draw is sorted fine-to-coarse. Reverse only this layer's range
        // so its fallback parents paint first, while the outer layer loop
        // preserves document painter order across layers with different LODs.
        for (auto drawKey = plan.draw.rbegin(); drawKey != plan.draw.rend();
             ++drawKey) {
            const RasterTileKey key = *drawKey;
            const RasterCacheKey cacheKey{sourceId, generation, key, profile};
            if (surface) {
                const auto residualRange =
                    rasters.tileElevationResidualRange(cacheKey);
                if (!residualRange) {
                    // Render-elevation tiles with no valid height also have
                    // fully invalid source alpha. Avoid submitting thousands
                    // of triangles only to discard every fragment.
                    continue;
                }
                const RasterBasePixelRect rect =
                    rasterTileBasePixelRect(metadata.levels[key.levelIndex],
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
            draw.mode =
                surface ? RasterDrawMode::Surface : RasterDrawMode::Flat;
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

    // A source the document no longer owns releases its decoded tiles, queued
    // reads, remembered failures, and GPU textures. Without this its in-flight
    // reads complete into a queue nothing drains, holding bytes outside every
    // accounted cache while the counters read zero.
    for (const RasterSourceId sourceId : knownRasterSources_) {
        if (std::ranges::find(rasterPlan.liveSources, sourceId) !=
            rasterPlan.liveSources.end()) {
            continue;
        }
        rasters.releaseSource(sourceId);
    }
    knownRasterSources_ = rasterPlan.liveSources;

    streamer.requeueReadyUploads(rasterPlan.decodedUploads);
    const std::vector<RasterCacheKey> readyUploads =
        streamer.takeReadyUploads(rasterMaximumFrameUploads);
    std::vector<RasterCacheKey> attemptedUploads;
    attemptedUploads.reserve(readyUploads.size());
    for (const RasterCacheKey &key : readyUploads) {
        const auto target = rasterPlan.uploadTargets.find(key.sourceId);
        if (target == rasterPlan.uploadTargets.end()) {
            continue;
        }
        const RasterTileData *tile = streamer.tile(key);
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
    rasters.retainLayers(rasterPlan.retainedLayers);
    const std::size_t uploaded =
        rasters.uploadPending(commandBuffer,
                              pending,
                              rasterPlan.protectedTiles,
                              rasterFrameUploadBytes);
    std::vector<RasterCacheKey> retryUploads;
    retryUploads.reserve(attemptedUploads.size() - uploaded);
    for (const RasterCacheKey &key : attemptedUploads) {
        if (!rasters.gpuResident(key)) {
            retryUploads.push_back(key);
        }
    }
    streamer.requeueReadyUploads(retryUploads);
    const auto surfaceDrawn = static_cast<std::size_t>(
        std::ranges::count_if(draws, [](const RasterLayerDraw &draw) {
            return draw.mode == RasterDrawMode::Surface;
        }));
    return RasterSubmission{
        .draws = std::move(draws),
        .uploaded = uploaded,
        .surfaceDrawn = surfaceDrawn,
        // Plans are built before upload. A successful upload becomes drawable
        // only after the next plan observes it as GPU-resident.
        .requiresContinuation = uploaded > 0,
    };
}

void FrameRenderer::record(QRhiCommandBuffer *commandBuffer,
                           QRhiRenderTarget *output,
                           const ViewportSettings &settings,
                           const bool edlActive,
                           const std::vector<BlockDraw> &draws,
                           const std::span<const VectorLayerDraw> vectorDraws,
                           const std::span<const RasterLayerDraw> rasterDraws)
{
    const QColor clear = QColor::fromRgbF(settings.backgroundColor.red,
                                          settings.backgroundColor.green,
                                          settings.backgroundColor.blue,
                                          1.0F);
    const QRhiDepthStencilClearValue depthClear{1.0F, 0};
    QRhiRenderTarget *pointTarget =
        edlActive ? edl.pointRenderTarget() : output;

    // Overlays record into whichever target holds the published depth:
    //   EDL off: begin widget pass -> points -> rasters -> vectors -> end
    //   EDL on:  begin EDL pass    -> points -> end
    //            begin widget pass -> composite -> rasters -> vectors -> end
    // Rasters receive no eye-dome lighting, which is why they draw after the
    // composite rather than alongside the points.
    commandBuffer->beginPass(pointTarget, clear, depthClear);
    points.recordDraws(commandBuffer, pointTarget, draws);
    if (!edlActive) {
        rasters.recordDraws(commandBuffer, pointTarget, rasterDraws);
        vectors.recordDraws(commandBuffer, pointTarget, vectorDraws);
    }
    commandBuffer->endPass();

    if (edlActive) {
        commandBuffer->beginPass(output, clear, depthClear);
        edl.recordComposite(commandBuffer, output);
        rasters.recordDraws(commandBuffer, output, rasterDraws);
        vectors.recordDraws(commandBuffer, output, vectorDraws);
        commandBuffer->endPass();
    }
}

void FrameRenderer::releaseResources()
{
    picker.releaseResources();
    vectors.releaseResources();
    rasters.releaseResources();
    points.releaseResources();
    edl.releaseResources();
    uploads.releaseResources();
}
} // namespace pci
