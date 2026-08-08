#include "renderer/rhi/RasterLayerRenderer.h"

#include "renderer/rhi/ShaderLoader.h"

#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace pci {
namespace {

constexpr std::size_t initialUniformCapacity = 16;

// Imagery and points at the same Z do not produce bit-identical depth: point
// depth comes from a sprite vertex path and raster depth from an interpolated
// quad, through different transforms with different rounding, and the EDL
// composite adds a clear-and-republish step. Without a bias the shared plane
// shimmers in a way that varies by driver and camera angle.
constexpr int rasterDepthBias = 1;
constexpr float rasterSlopeScaledDepthBias = 1.0F;

// The unit square as a triangle strip: (0,0), (1,0), (0,1), (1,1).
constexpr std::array<float, 8> unitQuadVertices{
    0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};

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

QShader shader(const QString &path)
{
    QString error;
    const QShader result = loadShaderResource(path, &error);
    if (!result.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    return result;
}

// Resolves the styled ramp key against the document's catalog. Portable raster
// code carries only the key; the stop table lives with the point catalog.
[[nodiscard]] std::shared_ptr<const std::vector<PointColorStop>>
resolveRamp(const PointColorMapCatalogSnapshotPtr &colorMaps,
            const std::string &key)
{
    if (!colorMaps || key.empty()) {
        return nullptr;
    }
    for (const PointColorMapDefinition &definition : colorMaps->definitions()) {
        if (definition.key != key || definition.stops.empty()) {
            continue;
        }
        return std::make_shared<const std::vector<PointColorStop>>(
            definition.stops.begin(), definition.stops.end());
    }
    return nullptr;
}

[[nodiscard]] RasterDecodeParameters
decodeFor(const RasterLayer &layer,
          const PointColorMapCatalogSnapshotPtr &colorMaps)
{
    const RasterLayerMetadata &metadata = layer.data->metadata();
    RasterDecodeParameters decode = metadata.defaultDisplay;
    if (layer.style.displayRange) {
        decode.displayRange = layer.style.displayRange;
    }
    if (auto ramp = resolveRamp(colorMaps, layer.style.colorRampKey)) {
        decode.colorRamp = std::move(ramp);
    }
    return decode;
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
    if (uniformStride < sizeof(RasterLayerUniform)) {
        throw std::invalid_argument(
            "raster uniform stride is smaller than the uniform block");
    }
    std::vector<std::byte> staging(draws.size() * uniformStride, std::byte{});
    for (std::size_t index = 0; index < draws.size(); ++index) {
        std::memcpy(staging.data() + index * uniformStride,
                    &draws[index].uniform,
                    sizeof(RasterLayerUniform));
    }
    return staging;
}

RasterLayerRenderer::~RasterLayerRenderer()
{
    releaseResources();
}

bool RasterLayerRenderer::ready() const noexcept
{
    return rhi_ != nullptr && pipeline_ && uniformBuffer_ && vertexBuffer_;
}

std::uint64_t RasterLayerRenderer::gpuBytes() const noexcept
{
    return gpuBytes_;
}

std::size_t RasterLayerRenderer::residentLayerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(layers_, [](const auto &entry) {
            return !entry.second.unavailable;
        }));
}

std::size_t RasterLayerRenderer::unavailableLayerCount() const noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(layers_, [](const auto &entry) {
            return entry.second.unavailable;
        }));
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
    if (!vertexBuffer_) {
        RhiResourcePtr<QRhiBuffer> buffer(
            rhi_->newBuffer(QRhiBuffer::Immutable,
                            QRhiBuffer::VertexBuffer,
                            sizeof(unitQuadVertices)));
        buffer->setName(QByteArrayLiteral("Raster unit quad"));
        requireCreated(buffer->create(), "raster vertex buffer");
        vertexBuffer_ = std::move(buffer);
        quadUploaded_ = false;
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
}

void RasterLayerRenderer::createUniformBuffer(const std::size_t drawCapacity)
{
    const auto stride =
        static_cast<quint32>(rhi_->ubufAligned(sizeof(RasterLayerUniform)));
    if (drawCapacity > std::numeric_limits<quint32>::max() / stride) {
        throw std::length_error(
            "raster uniform capacity exceeds QRhi buffer limits");
    }
    RhiResourcePtr<QRhiBuffer> buffer(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        stride * static_cast<quint32>(drawCapacity)));
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
            1, QRhiShaderResourceBinding::FragmentStage, nullptr, nullptr),
    });

    pipelineBindings_.reset();
    uniformBuffer_.reset();
    uniformBuffer_ = std::move(buffer);
    pipelineBindings_ = std::move(bindings);
    uniformStride_ = stride;
    uniformCapacity_ = drawCapacity;

    // Existing per-layer bindings reference the destroyed buffer, so they are
    // rebuilt on the next sync rather than left dangling.
    for (auto &[id, layer] : layers_) {
        layer.bindings.reset();
    }
}

void RasterLayerRenderer::ensureUniformCapacity(const std::size_t drawCount)
{
    if (drawCount <= uniformCapacity_) {
        return;
    }
    std::size_t capacity = std::max<std::size_t>(uniformCapacity_, 1);
    while (capacity < drawCount) {
        capacity *= 2;
    }
    createUniformBuffer(capacity);
    createPipeline(pipelineRenderPass_);
}

void RasterLayerRenderer::createPipeline(QRhiRenderPassDescriptor *renderPass)
{
    pipeline_.reset();
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(sizeof(float) * 2)});
    layout.setAttributes(
        {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)});

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

void RasterLayerRenderer::syncLayers(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const SceneLayer> layers,
    const PointColorMapCatalogSnapshotPtr &colorMaps)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("raster renderer is not ready to synchronize");
    }
    if (!quadUploaded_) {
        // Immutable buffers need one upload, and resource updates are only
        // legal outside an active render pass.
        RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
        updates->uploadStaticBuffer(vertexBuffer_.get(),
                                    unitQuadVertices.data());
        commandBuffer->resourceUpdate(updates.release());
        quadUploaded_ = true;
    }
    pruneLayers(layers);
    for (const SceneLayer &sceneLayer : layers) {
        const auto *raster = std::get_if<RasterLayerState>(&sceneLayer.payload);
        if (raster == nullptr || !raster->data || !raster->data->source) {
            continue;
        }
        ensureLayer(commandBuffer,
                    RasterLayer{
                        .id = sceneLayer.id,
                        .data = raster->data,
                        .visible = sceneLayer.visible,
                        .style = raster->style,
                        .renderGeneration = raster->renderGeneration,
                    },
                    colorMaps);
    }
}

void RasterLayerRenderer::ensureLayer(
    QRhiCommandBuffer *commandBuffer,
    const RasterLayer &layer,
    const PointColorMapCatalogSnapshotPtr &colorMaps)
{
    const auto existing = layers_.find(layer.id);
    if (existing != layers_.end() &&
        existing->second.sourceId == layer.data->sourceId &&
        existing->second.renderGeneration == layer.renderGeneration) {
        if (!existing->second.bindings && !existing->second.unavailable) {
            // The uniform buffer was recreated; rebind against the new one.
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
                    existing->second.texture.get(),
                    existing->second.nearest ? nearestSampler_.get()
                                             : linearSampler_.get()),
            });
            requireCreated(bindings->create(), "raster shader bindings");
            existing->second.bindings = std::move(bindings);
        }
        return;
    }
    if (existing != layers_.end()) {
        destroyLayer(existing->second);
        layers_.erase(existing);
    }

    const RasterLayerMetadata &metadata = layer.data->metadata();
    GpuLayer record;
    record.sourceId = layer.data->sourceId;
    record.renderGeneration = layer.renderGeneration;
    record.nearest =
        metadata.defaultDisplay.sampleKind == RasterSampleKind::Categorical;

    RasterStaticImage image;
    try {
        // Phase 1 reads on the render thread. The read is bounded by the
        // texture cap and by rasterBoundedBaseReadPixels, so it is a one-time
        // hitch rather than an unbounded scan; phase 2's streamer replaces it.
        image = layer.data->source->readStaticImage(
            static_cast<std::uint32_t>(
                std::max(1, rhi_->resourceLimit(QRhi::TextureSizeMax))),
            decodeFor(layer, colorMaps),
            std::stop_token{});
    } catch (const RasterReadError &) {
        // A source needing tiled rendering is recorded as unavailable so it is
        // reported once instead of retried on every frame.
        record.unavailable = true;
        layers_.emplace(layer.id, std::move(record));
        return;
    }

    RhiResourcePtr<QRhiTexture> texture(rhi_->newTexture(
        QRhiTexture::RGBA8,
        QSize(static_cast<int>(image.width), static_cast<int>(image.height))));
    texture->setName(QByteArrayLiteral("Raster layer texture"));
    requireCreated(texture->create(), "raster texture");

    QRhiTextureSubresourceUploadDescription subresource;
    subresource.setData(
        QByteArray(reinterpret_cast<const char *>(image.rgba.data()),
                   static_cast<qsizetype>(image.rgba.size())));
    subresource.setSourceSize(
        QSize(static_cast<int>(image.width), static_cast<int>(image.height)));
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    updates->uploadTexture(texture.get(),
                           QRhiTextureUploadDescription({0, 0, subresource}));
    commandBuffer->resourceUpdate(updates.release());

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
            record.nearest ? nearestSampler_.get() : linearSampler_.get()),
    });
    requireCreated(bindings->create(), "raster shader bindings");

    record.width = image.width;
    record.height = image.height;
    record.bytes = static_cast<std::uint64_t>(image.width) * image.height * 4;
    record.texture = std::move(texture);
    record.bindings = std::move(bindings);
    gpuBytes_ += record.bytes;
    layers_.emplace(layer.id, std::move(record));
}

void RasterLayerRenderer::updateUniforms(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const RasterLayerDraw> draws)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("raster renderer is not ready to update");
    }
    ensureUniformCapacity(draws.size());
    if (draws.empty()) {
        return;
    }
    const std::vector<std::byte> staging =
        stageRasterLayerUniforms(draws, uniformStride_);
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    updates->updateDynamicBuffer(uniformBuffer_.get(),
                                 0,
                                 static_cast<quint32>(staging.size()),
                                 staging.data());
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
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const RasterLayerDraw &draw = draws[index];
        const auto found = layers_.find(draw.layerId);
        if (found == layers_.end() || !found->second.bindings ||
            found->second.unavailable) {
            continue;
        }
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(index) * uniformStride_);
        commandBuffer->setGraphicsPipeline(pipeline_.get());
        commandBuffer->setShaderResources(
            found->second.bindings.get(), 1, &offset);
        const QRhiCommandBuffer::VertexInput vertexInput(vertexBuffer_.get(),
                                                         0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(4);
    }
}

void RasterLayerRenderer::destroyLayer(GpuLayer &layer) noexcept
{
    gpuBytes_ -= std::min(gpuBytes_, layer.bytes);
    layer.bytes = 0;
    layer.bindings.reset();
    layer.texture.reset();
}

void RasterLayerRenderer::pruneLayers(const std::span<const SceneLayer> layers)
{
    std::unordered_set<SceneLayerId> retained;
    retained.reserve(layers.size());
    for (const SceneLayer &layer : layers) {
        if (std::holds_alternative<RasterLayerState>(layer.payload)) {
            retained.insert(layer.id);
        }
    }
    for (auto entry = layers_.begin(); entry != layers_.end();) {
        if (retained.contains(entry->first)) {
            ++entry;
            continue;
        }
        destroyLayer(entry->second);
        entry = layers_.erase(entry);
    }
}

void RasterLayerRenderer::releaseResources()
{
    for (auto &[id, layer] : layers_) {
        destroyLayer(layer);
    }
    layers_.clear();
    gpuBytes_ = 0;
    pipeline_.reset();
    pipelineBindings_.reset();
    uniformBuffer_.reset();
    nearestSampler_.reset();
    linearSampler_.reset();
    vertexBuffer_.reset();
    quadUploaded_ = false;
    pipelineRenderPass_ = nullptr;
    uniformCapacity_ = 0;
    uniformStride_ = 0;
    rhi_ = nullptr;
}

} // namespace pci
