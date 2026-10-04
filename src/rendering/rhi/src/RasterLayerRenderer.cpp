#include <pci/rendering/rhi/RasterLayerRenderer.h>

#include <pci/rendering/rhi/ShaderLoader.h>
#include <pci/rendering/rhi/UniformStaging.h>

#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace pci {
namespace {

constexpr std::size_t initialUniformCapacity = 64;
constexpr std::size_t initialSurfaceUniformCapacity = 128;

// QRhi does not expose binding allocation sizes, so a conservative fixed
// amount is accounted per tile in addition to its texture bytes.
constexpr std::uint64_t rasterTileBindingOverheadBytes = 1024;
// An absolute floor, so a tiny configured budget still renders something.
constexpr std::uint64_t rasterMinimumResidentTiles = 16;

// Imagery and points at the same Z do not produce bit-identical depth: point
// depth comes from a sprite vertex path and raster depth from an interpolated
// quad, through different transforms with different rounding, and the EDL
// composite adds a clear-and-republish step. Without a bias the shared plane
// shimmers in a way that varies by driver and camera angle.
constexpr int rasterDepthBias = 1;
constexpr float rasterSlopeScaledDepthBias = 1.0F;

void requireCreated(const bool created, const char *resource)
{
    if (!created) {
        throw std::runtime_error("Could not create QRhi " +
                                 std::string(resource));
    }
}

QRhiGraphicsPipeline::TargetBlend premultipliedBlend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

constexpr std::size_t surfacePipelineIndex(const bool transparent,
                                           const bool fallback) noexcept
{
    return (transparent ? 2U : 0U) + (fallback ? 1U : 0U);
}

std::array<std::uint16_t, rasterSurfaceGridIndexCount> surfaceGridIndices()
{
    std::array<std::uint16_t, rasterSurfaceGridIndexCount> result{};
    std::size_t output = 0;
    for (std::uint32_t y = 0; y < rasterSurfaceGridCellsPerSide; ++y) {
        for (std::uint32_t x = 0; x < rasterSurfaceGridCellsPerSide; ++x) {
            const auto topLeft = static_cast<std::uint16_t>(
                y * rasterSurfaceGridVerticesPerSide + x);
            const auto topRight = static_cast<std::uint16_t>(topLeft + 1);
            const auto bottomLeft = static_cast<std::uint16_t>(
                topLeft + rasterSurfaceGridVerticesPerSide);
            const auto bottomRight = static_cast<std::uint16_t>(bottomLeft + 1);
            result[output++] = topLeft;
            result[output++] = bottomLeft;
            result[output++] = topRight;
            result[output++] = topRight;
            result[output++] = bottomLeft;
            result[output++] = bottomRight;
        }
    }
    return result;
}

QShader shader(const QString &path)
{
    QString error;
    const QShader result = loadShaderResource(path, &error);
    if (!result.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    return result;
}

} // namespace

RasterQuadTransform
rasterLayerQuadTransform(const RasterLayerMetadata &metadata,
                         const RasterLayerStyle &style,
                         const Vec3d eye) noexcept
{
    // Pixel edges, never centers, and the same six-term affine the whole
    // raster uses, so the quad lands exactly on the layer's bounds.
    const Vec3d topLeft = rasterPixelToWorld(metadata.geoTransform, 0.0, 0.0);
    const Vec3d topRight =
        rasterPixelToWorld(metadata.geoTransform, metadata.width, 0.0);
    const Vec3d bottomLeft =
        rasterPixelToWorld(metadata.geoTransform, 0.0, metadata.height);

    RasterQuadTransform transform;
    transform.origin =
        Vec3d{topLeft.x - eye.x, topLeft.y - eye.y, style.zOffset - eye.z};
    transform.edgeU = topRight - topLeft;
    transform.edgeV = bottomLeft - topLeft;
    return transform;
}

RasterQuadTransform rasterTileQuadTransform(const RasterLayerMetadata &metadata,
                                            const RasterLayerStyle &style,
                                            const RasterTileKey key,
                                            const Vec3d eye) noexcept
{
    if (key.levelIndex >= metadata.levels.size()) {
        return {};
    }
    const RasterBasePixelRect rect = rasterTileBasePixelRect(
        metadata.levels[key.levelIndex], key, metadata.width, metadata.height);
    const Vec3d topLeft = rasterPixelToWorld(
        metadata.geoTransform, rect.minimumPixel, rect.minimumLine);
    const Vec3d topRight = rasterPixelToWorld(
        metadata.geoTransform, rect.maximumPixel, rect.minimumLine);
    const Vec3d bottomLeft = rasterPixelToWorld(
        metadata.geoTransform, rect.minimumPixel, rect.maximumLine);

    RasterQuadTransform transform;
    transform.origin =
        Vec3d{topLeft.x - eye.x, topLeft.y - eye.y, style.zOffset - eye.z};
    transform.edgeU = topRight - topLeft;
    transform.edgeV = bottomLeft - topLeft;
    return transform;
}

std::array<float, 4> rasterTileUvRect(const std::uint16_t validWidth,
                                      const std::uint16_t validHeight) noexcept
{
    const auto stored = static_cast<float>(rasterStoredTilePixels);
    const auto gutter = static_cast<float>(rasterTileGutter);
    return {
        gutter / stored,
        gutter / stored,
        (gutter + static_cast<float>(validWidth)) / stored,
        (gutter + static_cast<float>(validHeight)) / stored,
    };
}

std::optional<Bounds3d>
rasterLayerCullBounds(const RasterLayerMetadata &metadata,
                      const RasterLayerStyle &style,
                      const FrameCamera &frame) noexcept
{
    const Bounds3d bounds = rasterSceneBounds(metadata, style);
    if (!bounds.valid() || !frame.culler.intersects(bounds)) {
        return std::nullopt;
    }
    return bounds;
}

std::vector<std::byte>
stageRasterLayerUniforms(const std::span<const RasterLayerDraw> draws,
                         const std::size_t uniformStride)
{
    std::vector<std::byte> staging;
    stageUniformRecords(draws,
                        uniformStride,
                        &RasterLayerDraw::uniform,
                        staging,
                        "raster uniform stride is smaller than the uniform "
                        "block",
                        "raster uniform staging size overflow");
    return staging;
}

std::vector<std::byte>
stageRasterSurfaceUniforms(const std::span<const RasterLayerDraw> draws,
                           const std::size_t uniformStride)
{
    std::vector<std::byte> staging;
    stageUniformRecords(draws,
                        uniformStride,
                        &RasterLayerDraw::surfaceUniform,
                        staging,
                        "surface uniform stride is smaller than the uniform "
                        "block",
                        "surface uniform staging size overflow");
    return staging;
}

RasterLayerRenderer::RasterLayerRenderer(const std::uint64_t gpuByteBudget)
    : gpuByteBudget_(gpuByteBudget)
{
}

RasterLayerRenderer::~RasterLayerRenderer()
{
    releaseResources();
}

bool RasterLayerRenderer::ready() const noexcept
{
    return rhi_ != nullptr && pipeline_ && uniformBuffer_;
}

bool RasterLayerRenderer::surfaceSupported() const noexcept
{
    return surfaceSupported_;
}

const std::string &RasterLayerRenderer::surfaceCapabilityReason() const noexcept
{
    return surfaceCapabilityReason_;
}

std::uint64_t RasterLayerRenderer::gpuBytes() const noexcept
{
    return gpuBytes_;
}

std::uint64_t RasterLayerRenderer::heightGpuBytes() const noexcept
{
    std::uint64_t result = 0;
    for (const auto &[key, tile] : tiles_) {
        result += tile.heightBytes;
    }
    return result;
}

std::uint64_t RasterLayerRenderer::gpuByteBudget() const noexcept
{
    return gpuByteBudget_;
}

std::uint64_t RasterLayerRenderer::peakGpuBytes() const noexcept
{
    return peakGpuBytes_;
}

void RasterLayerRenderer::ensureResources(QRhi *rhi,
                                          QRhiRenderPassDescriptor *renderPass)
{
    if (rhi == nullptr || renderPass == nullptr) {
        throw std::invalid_argument(
            "raster renderer requires a QRhi and render pass");
    }
    if (rhi_ != rhi) {
        releaseResources();
        rhi_ = rhi;
    }
    if (!linearSampler_) {
        RhiResourcePtr<QRhiSampler> sampler(
            rhi_->newSampler(QRhiSampler::Linear,
                             QRhiSampler::Linear,
                             QRhiSampler::None,
                             QRhiSampler::ClampToEdge,
                             QRhiSampler::ClampToEdge));
        requireCreated(sampler->create(), "raster linear sampler");
        linearSampler_ = std::move(sampler);
    }
    if (!nearestSampler_) {
        // Palette and categorical imagery must not be interpolated: blending
        // two indices produces a third, unrelated class.
        RhiResourcePtr<QRhiSampler> sampler(
            rhi_->newSampler(QRhiSampler::Nearest,
                             QRhiSampler::Nearest,
                             QRhiSampler::None,
                             QRhiSampler::ClampToEdge,
                             QRhiSampler::ClampToEdge));
        requireCreated(sampler->create(), "raster nearest sampler");
        nearestSampler_ = std::move(sampler);
    }
    if (!uniformBuffer_) {
        createUniformBuffer(initialUniformCapacity);
    }
    if (!pipeline_ || pipelineRenderPass_ != renderPass) {
        createPipeline(renderPass);
    }

    if (!rhi_->isTextureFormatSupported(QRhiTexture::R32F)) {
        releaseSurfaceResources();
        surfaceCapabilityReason_ =
            "This graphics backend does not support R32F textures.";
        return;
    }
    if (!rhi_->isFeatureSupported(QRhi::TexelFetch)) {
        releaseSurfaceResources();
        surfaceCapabilityReason_ =
            "This graphics backend does not support shader texel fetches.";
        return;
    }
    if (!surfaceSupported_ || surfacePipelineRenderPass_ != renderPass) {
        try {
            createSurfaceResources(renderPass);
            surfaceSupported_ = true;
            surfaceCapabilityReason_.clear();
        } catch (const std::exception &error) {
            releaseSurfaceResources();
            surfaceCapabilityReason_ =
                "Surface rendering could not be initialized: " +
                std::string(error.what());
        }
    }
}

void RasterLayerRenderer::createUniformBuffer(const std::size_t drawCapacity)
{
    const auto stride =
        static_cast<quint32>(rhi_->ubufAligned(sizeof(RasterLayerUniform)));
    const std::size_t byteSize = checkedUniformStagingByteSize(
        drawCapacity,
        stride,
        sizeof(RasterLayerUniform),
        std::numeric_limits<quint32>::max(),
        "raster renderer uniform stride is smaller than the uniform block",
        "raster uniform capacity exceeds QRhi buffer limits");
    RhiResourcePtr<QRhiBuffer> buffer(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        static_cast<quint32>(byteSize)));
    buffer->setName(QByteArrayLiteral("Raster layer uniforms"));
    requireCreated(buffer->create(), "raster uniform buffer");

    // The pipeline's layout binds a texture it never samples from; each layer
    // supplies its own bindings with a matching layout at draw time.
    RhiResourcePtr<QRhiShaderResourceBindings> bindings(
        rhi_->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            buffer.get(),
            static_cast<quint32>(sizeof(RasterLayerUniform))),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            nullptr,
            nullptr),
    });

    pipelineBindings_.reset();
    uniformBuffer_.reset();
    uniformBuffer_ = std::move(buffer);
    pipelineBindings_ = std::move(bindings);
    uniformStride_ = stride;
    uniformCapacity_ = drawCapacity;

    // Existing per-tile bindings reference the destroyed buffer. Dropping the
    // tiles is simpler than rebinding and costs one refill; a dangling binding
    // would be a device-lost crash.
    for (auto &[key, tile] : tiles_) {
        destroyTile(tile);
    }
    tiles_.clear();
}

void RasterLayerRenderer::ensureUniformCapacity(const std::size_t drawCount)
{
    if (drawCount <= uniformCapacity_) {
        return;
    }
    const std::size_t capacity =
        grownUniformDrawCapacity(uniformCapacity_, drawCount);
    createUniformBuffer(capacity);
    createPipeline(pipelineRenderPass_);
}

void RasterLayerRenderer::createPipeline(QRhiRenderPassDescriptor *renderPass)
{
    pipeline_.reset();
    QRhiVertexInputLayout layout;

    RhiResourcePtr<QRhiGraphicsPipeline> pipeline(rhi_->newGraphicsPipeline());
    pipeline->setName(QByteArrayLiteral("Raster layer pipeline"));
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    // Back-face culling is off because a negative pixel height or a rotated
    // transform can wind the quad either way.
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(true);
    // No depth write, so later raster layers provide painter ordering and
    // vectors remain the top overlay.
    pipeline->setDepthWrite(false);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setDepthBias(rasterDepthBias);
    pipeline->setSlopeScaledDepthBias(rasterSlopeScaledDepthBias);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex,
         shader(QStringLiteral(":/shaders/"
                               "raster.vert.qsb"))},
        {QRhiShaderStage::Fragment,
         shader(QStringLiteral(":/shaders/raster.frag.qsb"))},
    });
    pipeline->setTargetBlends({premultipliedBlend()});
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(pipelineBindings_.get());
    pipeline->setRenderPassDescriptor(renderPass);
    requireCreated(pipeline->create(), "raster graphics pipeline");
    pipeline_ = std::move(pipeline);
    pipelineRenderPass_ = renderPass;
}

void RasterLayerRenderer::createSurfaceResources(
    QRhiRenderPassDescriptor *renderPass)
{
    if (!surfaceUniformBuffer_) {
        createSurfaceUniformBuffer(initialSurfaceUniformCapacity);
    }
    if (!surfaceIndexBuffer_) {
        RhiResourcePtr<QRhiBuffer> indices(
            rhi_->newBuffer(QRhiBuffer::Immutable,
                            QRhiBuffer::IndexBuffer,
                            static_cast<quint32>(rasterSurfaceGridIndexCount *
                                                 sizeof(std::uint16_t))));
        indices->setName(QByteArrayLiteral("Raster surface grid indices"));
        requireCreated(indices->create(), "raster surface index buffer");
        surfaceIndexBuffer_ = std::move(indices);
        surfaceIndexUploaded_ = false;
    }
    createSurfacePipelines(renderPass);
}

void RasterLayerRenderer::createSurfaceUniformBuffer(
    const std::size_t drawCapacity)
{
    const auto stride =
        static_cast<quint32>(rhi_->ubufAligned(sizeof(RasterSurfaceUniform)));
    const std::size_t byteSize = checkedUniformStagingByteSize(
        drawCapacity,
        stride,
        sizeof(RasterSurfaceUniform),
        std::numeric_limits<quint32>::max(),
        "surface renderer uniform stride is smaller than the uniform block",
        "surface uniform capacity exceeds QRhi buffer limits");
    RhiResourcePtr<QRhiBuffer> buffer(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        static_cast<quint32>(byteSize)));
    buffer->setName(QByteArrayLiteral("Raster surface uniforms"));
    requireCreated(buffer->create(), "raster surface uniform buffer");

    RhiResourcePtr<QRhiShaderResourceBindings> bindings(
        rhi_->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            buffer.get(),
            static_cast<quint32>(sizeof(RasterSurfaceUniform))),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            nullptr,
            nullptr),
        QRhiShaderResourceBinding::sampledTexture(
            2,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            nullptr,
            nullptr),
    });

    surfacePipelineBindings_.reset();
    surfaceUniformBuffer_.reset();
    surfaceUniformBuffer_ = std::move(buffer);
    surfacePipelineBindings_ = std::move(bindings);
    surfaceUniformStride_ = stride;
    surfaceUniformCapacity_ = drawCapacity;
    rebuildSurfaceBindings();
}

void RasterLayerRenderer::ensureSurfaceUniformCapacity(
    const std::size_t drawCount)
{
    if (drawCount <= surfaceUniformCapacity_) {
        return;
    }
    const std::size_t capacity =
        grownUniformDrawCapacity(surfaceUniformCapacity_, drawCount);
    createSurfaceUniformBuffer(capacity);
    createSurfacePipelines(surfacePipelineRenderPass_);
}

void RasterLayerRenderer::rebuildSurfaceBindings()
{
    if (!surfaceUniformBuffer_) {
        return;
    }
    for (auto &[key, tile] : tiles_) {
        tile.surfaceBindings.reset();
        if (!tile.elevationTexture) {
            continue;
        }
        RhiResourcePtr<QRhiShaderResourceBindings> bindings(
            rhi_->newShaderResourceBindings());
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                surfaceUniformBuffer_.get(),
                static_cast<quint32>(sizeof(RasterSurfaceUniform))),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                tile.texture.get(),
                tile.nearest ? nearestSampler_.get() : linearSampler_.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                tile.elevationTexture.get(),
                nearestSampler_.get()),
        });
        requireCreated(bindings->create(),
                       "raster surface tile shader bindings");
        tile.surfaceBindings = std::move(bindings);
    }
}

void RasterLayerRenderer::createSurfacePipelines(
    QRhiRenderPassDescriptor *renderPass)
{
    if (!renderPass || !surfacePipelineBindings_) {
        throw std::invalid_argument(
            "surface pipeline requires a render pass and bindings");
    }
    for (auto &pipeline : surfacePipelines_) {
        pipeline.reset();
    }
    QRhiVertexInputLayout layout;
    for (const bool transparent : {false, true}) {
        for (const bool fallback : {false, true}) {
            RhiResourcePtr<QRhiGraphicsPipeline> pipeline(
                rhi_->newGraphicsPipeline());
            pipeline->setName(QByteArrayLiteral("Raster surface pipeline"));
            pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
            pipeline->setCullMode(QRhiGraphicsPipeline::None);
            pipeline->setDepthTest(true);
            pipeline->setDepthWrite(!transparent && !fallback);
            pipeline->setDepthOp(!transparent && !fallback
                                     ? QRhiGraphicsPipeline::Less
                                     : QRhiGraphicsPipeline::LessOrEqual);
            pipeline->setShaderStages({
                {QRhiShaderStage::Vertex,
                 shader(QStringLiteral(":/shaders/raster_surface.vert.qsb"))},
                {QRhiShaderStage::Fragment,
                 shader(QStringLiteral(":/shaders/raster_surface.frag.qsb"))},
            });
            if (transparent) {
                pipeline->setTargetBlends({premultipliedBlend()});
            }
            pipeline->setVertexInputLayout(layout);
            pipeline->setShaderResourceBindings(surfacePipelineBindings_.get());
            pipeline->setRenderPassDescriptor(renderPass);
            requireCreated(pipeline->create(),
                           "raster surface graphics pipeline");
            surfacePipelines_[surfacePipelineIndex(transparent, fallback)] =
                std::move(pipeline);
        }
    }
    surfacePipelineRenderPass_ = renderPass;
}

void RasterLayerRenderer::releaseSurfaceResources() noexcept
{
    surfaceSupported_ = false;
    for (auto &pipeline : surfacePipelines_) {
        pipeline.reset();
    }
    for (auto &[key, tile] : tiles_) {
        tile.surfaceBindings.reset();
        tile.elevationTexture.reset();
        gpuBytes_ -= std::min(gpuBytes_, tile.heightBytes);
        tile.bytes -= std::min(tile.bytes, tile.heightBytes);
        tile.heightBytes = 0;
    }
    surfacePipelineBindings_.reset();
    surfaceIndexBuffer_.reset();
    surfaceUniformBuffer_.reset();
    surfacePipelineRenderPass_ = nullptr;
    surfaceUniformStride_ = 0;
    surfaceUniformCapacity_ = 0;
    surfaceIndexUploaded_ = false;
}

std::size_t RasterLayerRenderer::uploadPending(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const RasterPendingUpload> pending,
    const std::span<const RasterCacheKey> protectedKeys,
    const std::uint64_t frameByteBudget)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("raster renderer is not ready to upload");
    }
    ++frameCounter_;

    // Hashed and ordered once for the whole pass rather than per tile.
    const std::unordered_set<RasterCacheKey> protectedSet(protectedKeys.begin(),
                                                          protectedKeys.end());
    EvictionPlan eviction = buildEvictionPlan(protectedSet);

    std::uint64_t spent = 0;
    std::size_t uploaded = 0;
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    if (surfaceSupported_ && surfaceIndexBuffer_ && !surfaceIndexUploaded_) {
        const auto indices = surfaceGridIndices();
        updates->uploadStaticBuffer(surfaceIndexBuffer_.get(), indices.data());
        surfaceIndexUploaded_ = true;
    }
    for (const RasterPendingUpload &entry : pending) {
        if (entry.tile == nullptr || tiles_.contains(entry.key)) {
            continue;
        }
        const bool withElevation =
            entry.key.profile == RasterTilePayloadProfile::RenderElevation &&
            surfaceSupported_;
        if (withElevation &&
            entry.tile->elevation.size() !=
                rasterStoredTilePixels * rasterStoredTilePixels) {
            continue;
        }
        const std::uint64_t heightBytes =
            withElevation
                ? static_cast<std::uint64_t>(entry.tile->elevation.size()) *
                          sizeof(float) +
                      rasterTileBindingOverheadBytes
                : 0;
        const auto bytes = static_cast<std::uint64_t>(entry.tile->rgba.size()) +
                           rasterTileBindingOverheadBytes + heightBytes;
        // Spread a large refinement across frames rather than stalling one.
        if (spent + bytes > frameByteBudget && uploaded > 0) {
            break;
        }
        if (!makeRoom(bytes, eviction)) {
            continue;
        }

        RhiResourcePtr<QRhiTexture> texture(
            rhi_->newTexture(QRhiTexture::RGBA8,
                             QSize(static_cast<int>(rasterStoredTilePixels),
                                   static_cast<int>(rasterStoredTilePixels))));
        texture->setName(QByteArrayLiteral("Raster tile"));
        requireCreated(texture->create(), "raster tile texture");

        QRhiTextureSubresourceUploadDescription subresource;
        subresource.setData(
            QByteArray(reinterpret_cast<const char *>(entry.tile->rgba.data()),
                       static_cast<qsizetype>(entry.tile->rgba.size())));
        subresource.setSourceSize(
            QSize(static_cast<int>(rasterStoredTilePixels),
                  static_cast<int>(rasterStoredTilePixels)));
        updates->uploadTexture(
            texture.get(), QRhiTextureUploadDescription({0, 0, subresource}));

        RhiResourcePtr<QRhiTexture> elevationTexture;
        if (withElevation) {
            elevationTexture.reset(rhi_->newTexture(
                QRhiTexture::R32F,
                QSize(static_cast<int>(rasterStoredTilePixels),
                      static_cast<int>(rasterStoredTilePixels))));
            elevationTexture->setName(
                QByteArrayLiteral("Raster elevation tile"));
            requireCreated(elevationTexture->create(),
                           "raster elevation texture");
            QRhiTextureSubresourceUploadDescription heightSubresource;
            heightSubresource.setData(QByteArray(
                reinterpret_cast<const char *>(entry.tile->elevation.data()),
                static_cast<qsizetype>(entry.tile->elevation.size() *
                                       sizeof(float))));
            heightSubresource.setSourceSize(
                QSize(static_cast<int>(rasterStoredTilePixels),
                      static_cast<int>(rasterStoredTilePixels)));
            updates->uploadTexture(
                elevationTexture.get(),
                QRhiTextureUploadDescription({0, 0, heightSubresource}));
        }

        RhiResourcePtr<QRhiShaderResourceBindings> bindings(
            rhi_->newShaderResourceBindings());
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                uniformBuffer_.get(),
                static_cast<quint32>(sizeof(RasterLayerUniform))),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                texture.get(),
                entry.nearest ? nearestSampler_.get() : linearSampler_.get()),
        });
        requireCreated(bindings->create(), "raster tile shader bindings");

        RhiResourcePtr<QRhiShaderResourceBindings> surfaceBindings;
        if (elevationTexture) {
            surfaceBindings.reset(rhi_->newShaderResourceBindings());
            surfaceBindings->setBindings({
                QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                    0,
                    QRhiShaderResourceBinding::VertexStage |
                        QRhiShaderResourceBinding::FragmentStage,
                    surfaceUniformBuffer_.get(),
                    static_cast<quint32>(sizeof(RasterSurfaceUniform))),
                QRhiShaderResourceBinding::sampledTexture(
                    1,
                    QRhiShaderResourceBinding::VertexStage |
                        QRhiShaderResourceBinding::FragmentStage,
                    texture.get(),
                    entry.nearest ? nearestSampler_.get()
                                  : linearSampler_.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    2,
                    QRhiShaderResourceBinding::VertexStage |
                        QRhiShaderResourceBinding::FragmentStage,
                    elevationTexture.get(),
                    nearestSampler_.get()),
            });
            requireCreated(surfaceBindings->create(),
                           "raster surface tile shader bindings");
        }

        tiles_.emplace(
            entry.key,
            GpuTile{
                .texture = std::move(texture),
                .bindings = std::move(bindings),
                .elevationTexture = std::move(elevationTexture),
                .surfaceBindings = std::move(surfaceBindings),
                .layerId = entry.layerId,
                .bytes = bytes,
                .heightBytes = heightBytes,
                .lastUsedFrame = frameCounter_,
                .validWidth = entry.tile->validWidth,
                .validHeight = entry.tile->validHeight,
                .hasTranslucentAlpha = entry.tile->hasTranslucentAlpha,
                .nearest = entry.nearest,
                .elevationMinimum = entry.tile->elevationMinimum,
                .elevationMaximum = entry.tile->elevationMaximum,
                .hasValidElevation = entry.tile->hasValidElevation,
            });
        gpuBytes_ += bytes;
        peakGpuBytes_ = std::max(peakGpuBytes_, gpuBytes_);
        spent += bytes;
        ++uploaded;
    }
    commandBuffer->resourceUpdate(updates.release());
    return uploaded;
}

RasterLayerRenderer::EvictionPlan RasterLayerRenderer::buildEvictionPlan(
    const std::unordered_set<RasterCacheKey> &protectedKeys) const
{
    EvictionPlan plan;
    plan.order.reserve(tiles_.size());
    for (const auto &[key, tile] : tiles_) {
        if (!protectedKeys.contains(key)) {
            plan.order.push_back(key);
        }
    }
    // Least recently used first. Tiles uploaded during this pass are never in
    // the plan, which is correct: they are the most recent by construction.
    std::ranges::sort(
        plan.order,
        [this](const RasterCacheKey &left, const RasterCacheKey &right) {
            return tiles_.at(left).lastUsedFrame <
                   tiles_.at(right).lastUsedFrame;
        });
    return plan;
}

bool RasterLayerRenderer::makeRoom(const std::uint64_t incoming,
                                   EvictionPlan &plan)
{
    // The same underflow-safe form the decoded cache uses: guard first, then
    // subtract, so a live budget decrease cannot wrap the comparison.
    if (incoming > gpuByteBudget_) {
        return false;
    }
    while (gpuBytes_ > gpuByteBudget_ - incoming) {
        if (plan.next >= plan.order.size()) {
            // Everything left resident is on screen this frame; decline rather
            // than evict what is being drawn.
            return false;
        }
        const auto victim = tiles_.find(plan.order[plan.next++]);
        if (victim == tiles_.end()) {
            continue;
        }
        destroyTile(victim->second);
        tiles_.erase(victim);
    }
    return true;
}

bool RasterLayerRenderer::gpuResident(const RasterCacheKey &key) const noexcept
{
    return tiles_.contains(key);
}

std::optional<RasterElevationRange>
RasterLayerRenderer::tileElevationResidualRange(
    const RasterCacheKey &key) const noexcept
{
    const auto tile = tiles_.find(key);
    if (tile == tiles_.end() || !tile->second.elevationTexture ||
        !tile->second.hasValidElevation) {
        return std::nullopt;
    }
    return RasterElevationRange{
        .minimum = tile->second.elevationMinimum,
        .maximum = tile->second.elevationMaximum,
    };
}

void RasterLayerRenderer::retainLayers(
    const std::span<const SceneLayerId> layerIds)
{
    for (auto entry = tiles_.begin(); entry != tiles_.end();) {
        if (std::ranges::find(layerIds, entry->second.layerId) !=
            layerIds.end()) {
            ++entry;
            continue;
        }
        destroyTile(entry->second);
        entry = tiles_.erase(entry);
    }
}

void RasterLayerRenderer::releaseSource(const RasterSourceId sourceId)
{
    for (auto entry = tiles_.begin(); entry != tiles_.end();) {
        if (entry->first.sourceId != sourceId) {
            ++entry;
            continue;
        }
        destroyTile(entry->second);
        entry = tiles_.erase(entry);
    }
}

void RasterLayerRenderer::setGpuByteBudget(
    const std::uint64_t bytes,
    const std::span<const RasterCacheKey> protectedKeys)
{
    gpuByteBudget_ = bytes;
    const std::unordered_set<RasterCacheKey> protectedSet(protectedKeys.begin(),
                                                          protectedKeys.end());
    EvictionPlan eviction = buildEvictionPlan(protectedSet);
    static_cast<void>(makeRoom(0, eviction));
}

std::size_t RasterLayerRenderer::residentTileCount() const noexcept
{
    return tiles_.size();
}

std::size_t RasterLayerRenderer::tileCapacity() const noexcept
{
    return tileCapacity(RasterTilePayloadProfile::ColorOnly);
}

std::size_t RasterLayerRenderer::tileCapacity(
    const RasterTilePayloadProfile profile,
    const std::optional<std::uint64_t> budget) const noexcept
{
    const std::uint64_t perTile =
        static_cast<std::uint64_t>(rasterStoredTileBytes) +
        rasterTileBindingOverheadBytes +
        (profile == RasterTilePayloadProfile::RenderElevation
             ? static_cast<std::uint64_t>(rasterStoredTilePixels) *
                       rasterStoredTilePixels * sizeof(float) +
                   rasterTileBindingOverheadBytes
             : 0);
    // A quarter is held back for fallback ancestors and in-flight uploads, so
    // the planner's target set cannot pin the whole budget.
    const std::uint64_t capacity = budget.value_or(gpuByteBudget_) / perTile;
    return static_cast<std::size_t>(std::max<std::uint64_t>(
        rasterMinimumResidentTiles, capacity - capacity / 4));
}

void RasterLayerRenderer::destroyTile(GpuTile &tile) noexcept
{
    gpuBytes_ -= std::min(gpuBytes_, tile.bytes);
    tile.bytes = 0;
    tile.bindings.reset();
    tile.surfaceBindings.reset();
    tile.elevationTexture.reset();
    tile.texture.reset();
}

void RasterLayerRenderer::updateUniforms(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const RasterLayerDraw> draws)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("raster renderer is not ready to update");
    }
    std::vector<RasterLayerDraw> flatDraws;
    std::vector<RasterLayerDraw> surfaceDraws;
    flatDraws.reserve(draws.size());
    surfaceDraws.reserve(draws.size());
    for (const RasterLayerDraw &draw : draws) {
        (draw.mode == RasterDrawMode::Surface ? surfaceDraws : flatDraws)
            .push_back(draw);
    }
    ensureUniformCapacity(flatDraws.size());
    if (!surfaceDraws.empty()) {
        if (!surfaceSupported_) {
            throw std::logic_error(
                "surface draws submitted to an unsupported renderer");
        }
        ensureSurfaceUniformCapacity(surfaceDraws.size());
    }
    if (draws.empty()) {
        return;
    }
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    if (!flatDraws.empty()) {
        stageUniformRecords(std::span<const RasterLayerDraw>(flatDraws),
                            uniformStride_,
                            &RasterLayerDraw::uniform,
                            flatUniformStaging_,
                            "raster uniform stride is smaller than the "
                            "uniform block",
                            "raster uniform staging size overflow",
                            std::numeric_limits<quint32>::max());
        updates->updateDynamicBuffer(
            uniformBuffer_.get(),
            0,
            static_cast<quint32>(flatUniformStaging_.size()),
            flatUniformStaging_.data());
    }
    if (!surfaceDraws.empty()) {
        stageUniformRecords(std::span<const RasterLayerDraw>(surfaceDraws),
                            surfaceUniformStride_,
                            &RasterLayerDraw::surfaceUniform,
                            surfaceUniformStaging_,
                            "surface uniform stride is smaller than the "
                            "uniform block",
                            "surface uniform staging size overflow",
                            std::numeric_limits<quint32>::max());
        updates->updateDynamicBuffer(
            surfaceUniformBuffer_.get(),
            0,
            static_cast<quint32>(surfaceUniformStaging_.size()),
            surfaceUniformStaging_.data());
    }
    commandBuffer->resourceUpdate(updates.release());
}

void RasterLayerRenderer::recordDraws(
    QRhiCommandBuffer *commandBuffer,
    QRhiRenderTarget *renderTarget,
    const std::span<const RasterLayerDraw> draws)
{
    if (!ready() || !commandBuffer || !renderTarget) {
        throw std::logic_error("raster renderer is not ready to record");
    }
    if (draws.empty()) {
        return;
    }
    commandBuffer->setViewport(
        QRhiViewport(0,
                     0,
                     static_cast<float>(renderTarget->pixelSize().width()),
                     static_cast<float>(renderTarget->pixelSize().height())));
    std::uint32_t flatIndex = 0;
    std::uint32_t surfaceIndex = 0;
    for (const RasterLayerDraw &draw : draws) {
        const std::uint32_t drawIndex =
            draw.mode == RasterDrawMode::Surface ? surfaceIndex++ : flatIndex++;
        const auto found = tiles_.find(draw.tileKey);
        if (found == tiles_.end()) {
            continue;
        }
        found->second.lastUsedFrame = frameCounter_;
        if (draw.mode == RasterDrawMode::Flat) {
            if (!found->second.bindings) {
                continue;
            }
            const QRhiCommandBuffer::DynamicOffset offset(
                0, drawIndex * uniformStride_);
            commandBuffer->setGraphicsPipeline(pipeline_.get());
            commandBuffer->setShaderResources(
                found->second.bindings.get(), 1, &offset);
            commandBuffer->draw(4);
            continue;
        }

        if (!surfaceSupported_ || !surfaceIndexUploaded_ ||
            !found->second.surfaceBindings || !surfaceIndexBuffer_) {
            continue;
        }
        const bool transparent =
            draw.transparent || found->second.hasTranslucentAlpha;
        const bool fallback =
            draw.surfaceRole == RasterSurfaceDrawRole::Fallback;
        const QRhiCommandBuffer::DynamicOffset offset(
            0, drawIndex * surfaceUniformStride_);
        commandBuffer->setGraphicsPipeline(
            surfacePipelines_[surfacePipelineIndex(transparent, fallback)]
                .get());
        commandBuffer->setShaderResources(
            found->second.surfaceBindings.get(), 1, &offset);
        commandBuffer->setVertexInput(0,
                                      0,
                                      nullptr,
                                      surfaceIndexBuffer_.get(),
                                      0,
                                      QRhiCommandBuffer::IndexUInt16);
        commandBuffer->drawIndexed(rasterSurfaceGridIndexCount);
    }
}

void RasterLayerRenderer::releaseResources()
{
    for (auto &[key, tile] : tiles_) {
        destroyTile(tile);
    }
    tiles_.clear();
    gpuBytes_ = 0;
    pipeline_.reset();
    pipelineBindings_.reset();
    uniformBuffer_.reset();
    releaseSurfaceResources();
    nearestSampler_.reset();
    linearSampler_.reset();
    pipelineRenderPass_ = nullptr;
    uniformCapacity_ = 0;
    uniformStride_ = 0;
    surfaceCapabilityReason_.clear();
    rhi_ = nullptr;
}

} // namespace pci
