#include <pci/rendering/rhi/VectorLayerRenderer.h>

#include <pci/rendering/rhi/ShaderLoader.h>
#include <pci/rendering/rhi/UniformStaging.h>

#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace pci {
namespace {

constexpr std::size_t initialUniformCapacity = 64;

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

std::optional<Bounds3d> vectorLayerCullBounds(const VectorLayerData &geometry,
                                              const VectorLayerStyle &style,
                                              const FrameCamera &frame) noexcept
{
    if (!geometry.bounds.valid()) {
        return std::nullopt;
    }
    Bounds3d bounds = geometry.bounds;
    bounds.minimum[2] += style.zOffset;
    bounds.maximum[2] += style.zOffset;

    double farthestDepth = -std::numeric_limits<double>::infinity();
    for (std::size_t x = 0; x != 2; ++x) {
        for (std::size_t y = 0; y != 2; ++y) {
            for (std::size_t z = 0; z != 2; ++z) {
                const Vec3d corner{bounds.minimum[0] +
                                       (bounds.maximum[0] - bounds.minimum[0]) *
                                           static_cast<double>(x),
                                   bounds.minimum[1] +
                                       (bounds.maximum[1] - bounds.minimum[1]) *
                                           static_cast<double>(y),
                                   bounds.minimum[2] +
                                       (bounds.maximum[2] - bounds.minimum[2]) *
                                           static_cast<double>(z)};
                farthestDepth = std::max(
                    farthestDepth, dot(corner - frame.eye, frame.forward));
            }
        }
    }
    if (!frame.orthographic && farthestDepth < frame.nearPlane) {
        return std::nullopt;
    }

    float pixelRadius = 0.0F;
    if (!geometry.segments.empty()) {
        pixelRadius =
            std::max(pixelRadius, style.strokeWidthPixels * 0.5F + 1.0F);
    }
    if (!geometry.markers.empty()) {
        pixelRadius = std::max(pixelRadius,
                               style.markerSizePixels * 0.5F +
                                   style.strokeWidthPixels * 0.5F + 1.0F);
    }
    const double depth =
        frame.orthographic
            ? 0.0
            : std::clamp(farthestDepth, frame.nearPlane, frame.farPlane);
    const double inflation = static_cast<double>(pixelRadius) *
                             frame.worldUnitsPerPixelAtDepth(depth);
    if (!std::isfinite(inflation)) {
        return std::nullopt;
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
        bounds.minimum[axis] -= inflation;
        bounds.maximum[axis] += inflation;
    }
    return frame.culler.intersects(bounds) ? std::optional<Bounds3d>(bounds)
                                           : std::nullopt;
}

std::vector<std::byte>
stageVectorLayerUniforms(const std::span<const VectorLayerDraw> draws,
                         const std::size_t uniformStride)
{
    std::vector<std::byte> result;
    stageUniformRecords(draws,
                        uniformStride,
                        &VectorLayerDraw::uniform,
                        result,
                        "vector uniform stride is smaller than "
                        "VectorLayerUniform",
                        "vector uniform staging size overflow");
    return result;
}

VectorLayerRenderer::~VectorLayerRenderer()
{
    releaseResources();
}

void VectorLayerRenderer::ensureResources(QRhi *rhi,
                                          QRhiRenderPassDescriptor *renderPass)
{
    if (!rhi || !renderPass) {
        throw std::invalid_argument("vector renderer requires QRhi resources");
    }
    if (rhi_ != rhi) {
        releaseResources();
        rhi_ = rhi;
        createResourceBindings(initialUniformCapacity);
    }
    if (!pipelines_[0] || pipelineRenderPass_ != renderPass) {
        createPipelines(renderPass);
    }
}

void VectorLayerRenderer::updateUniforms(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const VectorLayerDraw> draws)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("vector renderer is not ready to update");
    }
    ensureUniformCapacity(draws.size());
    if (draws.empty()) {
        return;
    }
    stageUniformRecords(draws,
                        uniformStride_,
                        &VectorLayerDraw::uniform,
                        uniformStaging_,
                        "vector uniform stride is smaller than "
                        "VectorLayerUniform",
                        "vector uniform staging size overflow",
                        std::numeric_limits<quint32>::max());
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    updates->updateDynamicBuffer(uniformBuffer_.get(),
                                 0,
                                 static_cast<quint32>(uniformStaging_.size()),
                                 uniformStaging_.data());
    commandBuffer->resourceUpdate(updates.release());
}

void VectorLayerRenderer::syncLayers(
    QRhiCommandBuffer *commandBuffer,
    const std::span<const SceneSnapshotLayer> layers)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("vector renderer is not ready to synchronize");
    }
    // Immutable geometry uploads must be submitted before beginPass(). QRhi
    // resource updates are not legal while graphics commands are recorded in
    // an active pass.
    pruneLayers(layers);
    for (const SceneSnapshotLayer &sceneLayer : layers) {
        const auto *vector =
            std::get_if<VectorLayerSnapshotState>(&sceneLayer.payload);
        if (vector && vector->data) {
            ensureLayer(commandBuffer,
                        {
                            .id = sceneLayer.id,
                            .data = vector->data,
                            .visible = sceneLayer.visible,
                            .style = vector->style,
                            .bindingGeneration = sceneLayer.bindingGeneration,
                        });
        }
    }
}

void VectorLayerRenderer::recordDraws(
    QRhiCommandBuffer *commandBuffer,
    QRhiRenderTarget *renderTarget,
    const std::span<const VectorLayerDraw> draws)
{
    if (!ready() || !commandBuffer || !renderTarget) {
        throw std::logic_error("vector renderer is not ready to record");
    }
    if (draws.empty()) {
        return;
    }

    commandBuffer->setViewport(
        QRhiViewport(0,
                     0,
                     static_cast<float>(renderTarget->pixelSize().width()),
                     static_cast<float>(renderTarget->pixelSize().height())));
    for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
        const VectorLayerDraw &draw = draws[drawIndex];
        if (!draw.layer.data) {
            continue;
        }
        const auto found = layers_.find(draw.layerId);
        if (found == layers_.end()) {
            continue;
        }
        const GpuLayer &layer = found->second;
        const VectorDepthMode depth = draw.layer.style.alwaysOnTop
                                          ? VectorDepthMode::AlwaysOnTop
                                          : VectorDepthMode::Tested;
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(drawIndex) * uniformStride_);

        for (const GpuFillBatch &batch : layer.fills) {
            if (!batch.vertices || !batch.indices || batch.indexCount == 0) {
                continue;
            }
            commandBuffer->setGraphicsPipeline(
                pipelines_[vectorPipelineIndex(VectorPrimitive::Fill, depth)]
                    .get());
            commandBuffer->setShaderResources(
                shaderBindings_.get(), 1, &offset);
            const QRhiCommandBuffer::VertexInput vertexInput(
                batch.vertices.get(), 0);
            commandBuffer->setVertexInput(0,
                                          1,
                                          &vertexInput,
                                          batch.indices.get(),
                                          0,
                                          QRhiCommandBuffer::IndexUInt16);
            commandBuffer->drawIndexed(batch.indexCount);
        }
        if (layer.segments && layer.segmentCount != 0) {
            commandBuffer->setGraphicsPipeline(
                pipelines_[vectorPipelineIndex(VectorPrimitive::Line, depth)]
                    .get());
            commandBuffer->setShaderResources(
                shaderBindings_.get(), 1, &offset);
            const QRhiCommandBuffer::VertexInput vertexInput(
                layer.segments.get(), 0);
            commandBuffer->setVertexInput(0, 1, &vertexInput);
            commandBuffer->draw(4, layer.segmentCount);
        }
        if (layer.markers && layer.markerCount != 0) {
            commandBuffer->setGraphicsPipeline(
                pipelines_[vectorPipelineIndex(VectorPrimitive::Marker, depth)]
                    .get());
            commandBuffer->setShaderResources(
                shaderBindings_.get(), 1, &offset);
            const QRhiCommandBuffer::VertexInput vertexInput(
                layer.markers.get(), 0);
            commandBuffer->setVertexInput(0, 1, &vertexInput);
            commandBuffer->draw(4, layer.markerCount);
        }
    }
}

void VectorLayerRenderer::releaseResources()
{
    for (auto &[id, layer] : layers_) {
        static_cast<void>(id);
        destroyLayer(layer);
    }
    layers_.clear();
    for (auto &pipeline : pipelines_) {
        pipeline.reset();
    }
    shaderBindings_.reset();
    uniformBuffer_.reset();
    pipelineRenderPass_ = nullptr;
    uniformStride_ = 0;
    uniformCapacity_ = 0;
    gpuBytes_ = 0;
    rhi_ = nullptr;
}

bool VectorLayerRenderer::ready() const noexcept
{
    return rhi_ && uniformBuffer_ && shaderBindings_ && pipelines_[0];
}

std::uint64_t VectorLayerRenderer::gpuBytes() const noexcept
{
    return gpuBytes_;
}

void VectorLayerRenderer::createResourceBindings(const std::size_t drawCapacity)
{
    const quint32 stride =
        static_cast<quint32>(rhi_->ubufAligned(sizeof(VectorLayerUniform)));
    const std::size_t byteSize = checkedUniformStagingByteSize(
        drawCapacity,
        stride,
        sizeof(VectorLayerUniform),
        std::numeric_limits<quint32>::max(),
        "vector renderer uniform stride is smaller than VectorLayerUniform",
        "vector uniform capacity exceeds QRhi buffer limits");
    RhiResourcePtr<QRhiBuffer> buffer(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        static_cast<quint32>(byteSize)));
    buffer->setName(QByteArrayLiteral("Vector layer uniforms"));
    if (!buffer->create()) {
        throw std::runtime_error("Could not create QRhi vector uniform buffer");
    }
    RhiResourcePtr<QRhiShaderResourceBindings> bindings(
        rhi_->newShaderResourceBindings());
    bindings->setBindings(
        {QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            buffer.get(),
            static_cast<quint32>(sizeof(VectorLayerUniform)))});
    if (!bindings->create()) {
        throw std::runtime_error(
            "Could not create QRhi vector shader resource bindings");
    }
    shaderBindings_.reset();
    uniformBuffer_.reset();
    uniformBuffer_ = std::move(buffer);
    shaderBindings_ = std::move(bindings);
    uniformStride_ = stride;
    uniformCapacity_ = drawCapacity;
}

void VectorLayerRenderer::ensureUniformCapacity(const std::size_t drawCount)
{
    if (drawCount <= uniformCapacity_) {
        return;
    }
    const std::size_t capacity =
        grownUniformDrawCapacity(uniformCapacity_, drawCount);
    createResourceBindings(capacity);
    createPipelines(pipelineRenderPass_);
}

void VectorLayerRenderer::createPipelines(QRhiRenderPassDescriptor *renderPass)
{
    for (auto &pipeline : pipelines_) {
        pipeline.reset();
    }
    const std::array<QShader, 3> vertexShaders{
        shader(QStringLiteral(":/shaders/vector_fill.vert.qsb")),
        shader(QStringLiteral(":/shaders/vector_line.vert.qsb")),
        shader(QStringLiteral(":/shaders/vector_marker.vert.qsb"))};
    const std::array<QShader, 3> fragmentShaders{
        shader(QStringLiteral(":/shaders/vector_fill.frag.qsb")),
        shader(QStringLiteral(":/shaders/vector_line.frag.qsb")),
        shader(QStringLiteral(":/shaders/vector_marker.frag.qsb"))};
    for (std::size_t primitive = 0; primitive != 3; ++primitive) {
        QRhiVertexInputLayout layout;
        if (primitive == static_cast<std::size_t>(VectorPrimitive::Line)) {
            layout.setBindings({QRhiVertexInputBinding(
                static_cast<quint32>(sizeof(VectorSegment2f)),
                QRhiVertexInputBinding::PerInstance)});
            layout.setAttributes({QRhiVertexInputAttribute(
                0, 0, QRhiVertexInputAttribute::Float4, 0)});
        } else {
            const QRhiVertexInputBinding::Classification classification =
                primitive == static_cast<std::size_t>(VectorPrimitive::Marker)
                    ? QRhiVertexInputBinding::PerInstance
                    : QRhiVertexInputBinding::PerVertex;
            layout.setBindings({QRhiVertexInputBinding(
                static_cast<quint32>(sizeof(VectorVertex2f)), classification)});
            layout.setAttributes({QRhiVertexInputAttribute(
                0, 0, QRhiVertexInputAttribute::Float2, 0)});
        }
        for (std::size_t mode = 0; mode != 2; ++mode) {
            RhiResourcePtr<QRhiGraphicsPipeline> pipeline(
                rhi_->newGraphicsPipeline());
            pipeline->setName(QByteArrayLiteral("Vector overlay pipeline"));
            pipeline->setTopology(
                primitive == static_cast<std::size_t>(VectorPrimitive::Fill)
                    ? QRhiGraphicsPipeline::Triangles
                    : QRhiGraphicsPipeline::TriangleStrip);
            const bool alwaysOnTop =
                mode == static_cast<std::size_t>(VectorDepthMode::AlwaysOnTop);
            pipeline->setCullMode(QRhiGraphicsPipeline::None);
            pipeline->setDepthTest(!alwaysOnTop);
            pipeline->setDepthWrite(false);
            pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            pipeline->setShaderStages({
                {QRhiShaderStage::Vertex, vertexShaders[primitive]},
                {QRhiShaderStage::Fragment, fragmentShaders[primitive]},
            });
            pipeline->setTargetBlends({premultipliedBlend()});
            pipeline->setVertexInputLayout(layout);
            pipeline->setShaderResourceBindings(shaderBindings_.get());
            pipeline->setRenderPassDescriptor(renderPass);
            requireCreated(pipeline->create(), "vector graphics pipeline");
            pipelines_[primitive * 2 + mode] = std::move(pipeline);
        }
    }
    pipelineRenderPass_ = renderPass;
}

void VectorLayerRenderer::ensureLayer(QRhiCommandBuffer *commandBuffer,
                                      const VectorLayer &layer)
{
    const auto existing = layers_.find(layer.id);
    if (existing != layers_.end() &&
        existing->second.data == layer.data.get()) {
        return;
    }
    if (existing != layers_.end()) {
        gpuBytes_ -= existing->second.bytes;
        destroyLayer(existing->second);
        layers_.erase(existing);
    }
    GpuLayer gpu;
    gpu.data = layer.data.get();
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    auto upload =
        [&]<typename T>(const std::vector<T> &values,
                        QRhiBuffer::UsageFlags usage,
                        const char *name) -> RhiResourcePtr<QRhiBuffer> {
        if (values.empty())
            return {};
        if (values.size() > std::numeric_limits<quint32>::max() / sizeof(T)) {
            throw std::length_error("vector buffer exceeds QRhi limits");
        }
        RhiResourcePtr<QRhiBuffer> buffer(
            rhi_->newBuffer(QRhiBuffer::Immutable,
                            usage,
                            static_cast<quint32>(values.size() * sizeof(T))));
        buffer->setName(QByteArray(name));
        if (!buffer->create()) {
            throw std::runtime_error(
                "Could not create QRhi vector geometry buffer");
        }
        return buffer;
    };
    gpu.fills.reserve(layer.data->fillBatches.size());
    for (const VectorFillBatch &batch : layer.data->fillBatches) {
        GpuFillBatch target;
        target.vertices = upload(
            batch.vertices, QRhiBuffer::VertexBuffer, "Vector fill vertices");
        target.indices = upload(
            batch.indices, QRhiBuffer::IndexBuffer, "Vector fill indices");
        target.indexCount = static_cast<quint32>(batch.indices.size());
        target.bytes = static_cast<std::uint64_t>(batch.vertices.size()) *
                           sizeof(VectorVertex2f) +
                       static_cast<std::uint64_t>(batch.indices.size()) *
                           sizeof(std::uint16_t);
        gpu.bytes += target.bytes;
        gpu.fills.push_back(std::move(target));
    }
    gpu.segments = upload(
        layer.data->segments, QRhiBuffer::VertexBuffer, "Vector segments");
    gpu.markers =
        upload(layer.data->markers, QRhiBuffer::VertexBuffer, "Vector markers");
    gpu.segmentCount = static_cast<quint32>(layer.data->segments.size());
    gpu.markerCount = static_cast<quint32>(layer.data->markers.size());
    gpu.bytes += static_cast<std::uint64_t>(layer.data->segments.size()) *
                     sizeof(VectorSegment2f) +
                 static_cast<std::uint64_t>(layer.data->markers.size()) *
                     sizeof(VectorVertex2f);

    const std::uint64_t layerBytes = gpu.bytes;
    const auto [resident, inserted] = layers_.emplace(layer.id, std::move(gpu));
    if (!inserted) {
        throw std::logic_error(
            "vector layer became resident during synchronization");
    }
    try {
        GpuLayer &residentLayer = resident->second;
        for (std::size_t index = 0; index < residentLayer.fills.size();
             ++index) {
            const VectorFillBatch &source = layer.data->fillBatches[index];
            const GpuFillBatch &target = residentLayer.fills[index];
            if (target.vertices) {
                updates->uploadStaticBuffer(target.vertices.get(),
                                            source.vertices.data());
            }
            if (target.indices) {
                updates->uploadStaticBuffer(target.indices.get(),
                                            source.indices.data());
            }
        }
        if (residentLayer.segments) {
            updates->uploadStaticBuffer(residentLayer.segments.get(),
                                        layer.data->segments.data());
        }
        if (residentLayer.markers) {
            updates->uploadStaticBuffer(residentLayer.markers.get(),
                                        layer.data->markers.data());
        }
        commandBuffer->resourceUpdate(updates.release());
    } catch (...) {
        updates.reset();
        layers_.erase(resident);
        throw;
    }
    gpuBytes_ += layerBytes;
}

void VectorLayerRenderer::destroyLayer(GpuLayer &layer) noexcept
{
    layer = {};
}

void VectorLayerRenderer::pruneLayers(
    const std::span<const SceneSnapshotLayer> layers)
{
    std::unordered_set<SceneLayerId> active;
    active.reserve(layers.size());
    for (const SceneSnapshotLayer &layer : layers) {
        if (std::holds_alternative<VectorLayerSnapshotState>(layer.payload)) {
            active.insert(layer.id);
        }
    }
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (active.contains(it->first)) {
            ++it;
            continue;
        }
        gpuBytes_ -= it->second.bytes;
        destroyLayer(it->second);
        it = layers_.erase(it);
    }
}

} // namespace pci
