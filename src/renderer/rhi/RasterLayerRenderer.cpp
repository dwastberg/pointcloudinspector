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

constexpr std::size_t initialUniformCapacity = 64;

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

std::uint64_t RasterLayerRenderer::gpuBytes() const noexcept
{
    return gpuBytes_;
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
    for (const RasterPendingUpload &entry : pending) {
        if (entry.tile == nullptr || tiles_.contains(entry.key)) {
            continue;
        }
        const auto bytes = static_cast<std::uint64_t>(entry.tile->rgba.size()) +
                           rasterTileBindingOverheadBytes;
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

        tiles_.emplace(entry.key,
                       GpuTile{
                           .texture = std::move(texture),
                           .bindings = std::move(bindings),
                           .layerId = entry.layerId,
                           .bytes = bytes,
                           .lastUsedFrame = frameCounter_,
                           .validWidth = entry.tile->validWidth,
                           .validHeight = entry.tile->validHeight,
                       });
        gpuBytes_ += bytes;
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
    const std::uint64_t perTile =
        static_cast<std::uint64_t>(rasterStoredTileBytes) +
        rasterTileBindingOverheadBytes;
    // A quarter is held back for fallback ancestors and in-flight uploads, so
    // the planner's target set cannot pin the whole budget.
    const std::uint64_t capacity = gpuByteBudget_ / perTile;
    return static_cast<std::size_t>(std::max<std::uint64_t>(
        rasterMinimumResidentTiles, capacity - capacity / 4));
}

void RasterLayerRenderer::destroyTile(GpuTile &tile) noexcept
{
    gpuBytes_ -= std::min(gpuBytes_, tile.bytes);
    tile.bytes = 0;
    tile.bindings.reset();
    tile.texture.reset();
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
        const auto found = tiles_.find(draw.tileKey);
        if (found == tiles_.end() || !found->second.bindings) {
            continue;
        }
        found->second.lastUsedFrame = frameCounter_;
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(index) * uniformStride_);
        commandBuffer->setGraphicsPipeline(pipeline_.get());
        commandBuffer->setShaderResources(
            found->second.bindings.get(), 1, &offset);
        commandBuffer->draw(4);
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
    nearestSampler_.reset();
    linearSampler_.reset();
    pipelineRenderPass_ = nullptr;
    uniformCapacity_ = 0;
    uniformStride_ = 0;
    rhi_ = nullptr;
}

} // namespace pci
