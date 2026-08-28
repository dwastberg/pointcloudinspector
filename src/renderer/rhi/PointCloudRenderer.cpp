#include "renderer/rhi/PointCloudRenderer.h"

#include "pointcloud/GpuPoint.h"
#include "renderer/PointColorMapAtlas.h"
#include "renderer/rhi/ShaderLoader.h"

#include <QColor>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace pci {
namespace {

// Color sources describe packed vertex attributes and therefore remain part
// of the shader ABI. Color maps themselves are catalog/LUT data and have no
// corresponding GLSL constants.
static_assert(static_cast<int>(PointColorSource::Rgb) == 0);
static_assert(static_cast<int>(PointColorSource::X) == 1);
static_assert(static_cast<int>(PointColorSource::Y) == 2);
static_assert(static_cast<int>(PointColorSource::Z) == 3);
static_assert(static_cast<int>(PointColorSource::Intensity) == 4);
static_assert(static_cast<int>(PointColorSource::Classification) == 5);
static_assert(static_cast<int>(PointColorSource::ReturnNumber) == 6);
static_assert(static_cast<int>(PointColorSource::NumberOfReturns) == 7);

constexpr std::size_t initialDrawCapacity = 256;

void requireCreated(const bool created, const char *resource)
{
    if (!created) {
        throw std::runtime_error("Could not create QRhi " +
                                 std::string(resource));
    }
}

} // namespace

ScalarNormalization blockScalarNormalization(const double blockOrigin,
                                             const double blockScale,
                                             const double layerMinimum,
                                             const double layerMaximum) noexcept
{
    const double span = layerMaximum - layerMinimum;
    if (!std::isfinite(blockOrigin) || !std::isfinite(blockScale) ||
        !std::isfinite(layerMinimum) || !std::isfinite(layerMaximum) ||
        span <= 0.0 || blockScale < 0.0) {
        return {};
    }

    const double offset = (blockOrigin - layerMinimum) / span;
    const double step = blockScale / span;
    if (!std::isfinite(offset) || !std::isfinite(step)) {
        return {};
    }
    return {
        .offset = static_cast<float>(offset),
        .step = static_cast<float>(step),
    };
}

std::size_t
grownUniformDrawCapacity(const std::size_t currentCapacity,
                         const std::size_t requiredDrawCount) noexcept
{
    if (requiredDrawCount <= currentCapacity) {
        return currentCapacity;
    }
    if (currentCapacity == 0) {
        return requiredDrawCount;
    }

    std::size_t capacity = currentCapacity;
    while (capacity < requiredDrawCount) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            // The exact requirement is representable even when the preferred
            // power-of-two growth would overflow.
            return requiredDrawCount;
        }
        capacity *= 2;
    }
    return capacity;
}

float largestDrawPointSizePixels(
    const std::span<const BlockDraw> draws) noexcept
{
    float largest = 1.0F;
    for (const BlockDraw &draw : draws) {
        if (std::isfinite(draw.uniform.pointSize)) {
            largest = std::max(largest, draw.uniform.pointSize);
        }
    }
    return largest;
}

std::vector<std::byte>
stageBlockUniforms(const std::span<const BlockDraw> draws,
                   const std::size_t uniformStride)
{
    if (uniformStride < sizeof(BlockUniform)) {
        throw std::invalid_argument(
            "uniform stride is smaller than BlockUniform");
    }
    if (!draws.empty() &&
        uniformStride >
            std::numeric_limits<std::size_t>::max() / draws.size()) {
        throw std::length_error("uniform staging size overflow");
    }
    std::vector<std::byte> staging(uniformStride * draws.size());
    for (std::size_t index = 0; index < draws.size(); ++index) {
        std::memcpy(staging.data() + index * uniformStride,
                    &draws[index].uniform,
                    sizeof(BlockUniform));
    }
    return staging;
}

PointCloudRenderer::PointCloudRenderer(
    PointColorMapCatalogSnapshotPtr colorMaps)
    : colorMaps_(colorMaps ? std::move(colorMaps)
                           : createBuiltInPointColorMapCatalog())
{
}

PointCloudRenderer::~PointCloudRenderer()
{
    releaseResources();
}

void PointCloudRenderer::ensureResources(
    QRhi *rhi, QRhiRenderPassDescriptor *renderPassDescriptor)
{
    if (!rhi || !renderPassDescriptor) {
        throw std::invalid_argument("point renderer requires QRhi resources");
    }
    if (rhi_ != rhi) {
        releaseResources();
        rhi_ = rhi;
        createColorMapResources();
        createResourceBindings(initialDrawCapacity);
    }
    if (!pipeline_ || pipelineRenderPass_ != renderPassDescriptor) {
        createPipeline(renderPassDescriptor);
    }
}

void PointCloudRenderer::updateUniforms(QRhiCommandBuffer *commandBuffer,
                                        const std::vector<BlockDraw> &draws)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("point renderer is not ready to update");
    }
    ensureUniformCapacity(draws.size());
    uniformUpdateOperationCount_ = 0;
    if (draws.empty() && !colorMapUploadPending_) {
        return;
    }
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    if (colorMapUploadPending_) {
        updates->uploadTexture(colorMapTexture_.get(),
                               buildPointColorMapAtlas(*colorMaps_));
        colorMapUploadPending_ = false;
    }
    std::vector<std::byte> staging;
    if (!draws.empty()) {
        staging = stageBlockUniforms(draws, uniformStride_);
        updates->updateDynamicBuffer(uniformBuffer_.get(),
                                     0,
                                     static_cast<quint32>(staging.size()),
                                     staging.data());
    }
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkBegin(QByteArrayLiteral("Point uniforms"));
#endif
    commandBuffer->resourceUpdate(updates.release());
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkEnd();
#endif
    uniformUpdateOperationCount_ = draws.empty() ? 0 : 1;
}

void PointCloudRenderer::recordDraws(QRhiCommandBuffer *commandBuffer,
                                     QRhiRenderTarget *renderTarget,
                                     const std::vector<BlockDraw> &draws)
{
    if (!ready() || !commandBuffer || !renderTarget) {
        throw std::logic_error("point renderer is not ready to record");
    }

#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkBegin(QByteArrayLiteral("Main point pass"));
#endif
    commandBuffer->setGraphicsPipeline(pipeline_.get());
    commandBuffer->setViewport(
        QRhiViewport(0,
                     0,
                     static_cast<float>(renderTarget->pixelSize().width()),
                     static_cast<float>(renderTarget->pixelSize().height())));
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(i) * uniformStride_);
        commandBuffer->setShaderResources(shaderBindings_.get(), 1, &offset);
        const QRhiCommandBuffer::VertexInput vertexInput(draws[i].buffer, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(draws[i].pointCount);
    }
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkEnd();
#endif
}

QRhiShaderResourceBindings *PointCloudRenderer::shaderBindings() const noexcept
{
    return shaderBindings_.get();
}

quint32 PointCloudRenderer::uniformStride() const noexcept
{
    return uniformStride_;
}

std::size_t PointCloudRenderer::uniformDrawCapacity() const noexcept
{
    return uniformCapacity_;
}

std::uint64_t PointCloudRenderer::uniformCapacityGrowthCount() const noexcept
{
    return uniformCapacityGrowthCount_;
}

std::uint64_t PointCloudRenderer::uniformUpdateOperationCount() const noexcept
{
    return uniformUpdateOperationCount_;
}

bool PointCloudRenderer::ready() const noexcept
{
    return rhi_ && uniformBuffer_ && shaderBindings_ && pipeline_;
}

void PointCloudRenderer::releaseResources()
{
    pipeline_.reset();
    pipelineRenderPass_ = nullptr;
    shaderBindings_.reset();
    uniformBuffer_.reset();
    colorMapSampler_.reset();
    colorMapTexture_.reset();
    colorMapUploadPending_ = false;
    uniformCapacity_ = 0;
    uniformCapacityGrowthCount_ = 0;
    uniformUpdateOperationCount_ = 0;
    rhi_ = nullptr;
}

void PointCloudRenderer::createColorMapResources()
{
    const QImage atlas = buildPointColorMapAtlas(*colorMaps_);
    colorMapTexture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, atlas.size()));
    colorMapTexture_->setName(QByteArrayLiteral("Point color-map atlas"));
    if (!colorMapTexture_->create()) {
        colorMapTexture_.reset();
        throw std::runtime_error(
            "Could not create QRhi color-map atlas texture");
    }
    colorMapSampler_.reset(rhi_->newSampler(QRhiSampler::Linear,
                                            QRhiSampler::Linear,
                                            QRhiSampler::None,
                                            QRhiSampler::ClampToEdge,
                                            QRhiSampler::ClampToEdge));
    colorMapSampler_->setName(QByteArrayLiteral("Point color-map sampler"));
    if (!colorMapSampler_->create()) {
        colorMapSampler_.reset();
        colorMapTexture_.reset();
        throw std::runtime_error("Could not create QRhi color-map sampler");
    }
    colorMapUploadPending_ = true;
}

void PointCloudRenderer::createResourceBindings(const std::size_t drawCapacity)
{
    const quint32 newUniformStride =
        static_cast<quint32>(rhi_->ubufAligned(sizeof(BlockUniform)));
    if (drawCapacity > std::numeric_limits<quint32>::max() / newUniformStride) {
        throw std::length_error(
            "point renderer uniform capacity exceeds QRhi buffer limits");
    }
    RhiResourcePtr<QRhiBuffer> newUniformBuffer(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        newUniformStride * static_cast<quint32>(drawCapacity)));
    newUniformBuffer->setName(QByteArrayLiteral("Block uniforms"));
    if (!newUniformBuffer->create()) {
        throw std::runtime_error("Could not create QRhi block uniform buffer");
    }

    RhiResourcePtr<QRhiShaderResourceBindings> newShaderBindings(
        rhi_->newShaderResourceBindings());
    newShaderBindings->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0,
            QRhiShaderResourceBinding::VertexStage,
            newUniformBuffer.get(),
            static_cast<quint32>(sizeof(BlockUniform))),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::FragmentStage,
            colorMapTexture_.get(),
            colorMapSampler_.get()),
    });
    if (!newShaderBindings->create()) {
        throw std::runtime_error(
            "Could not create QRhi shader resource bindings");
    }

    // Commit only after both replacements are valid. A failed capacity
    // expansion therefore leaves the renderer's current resources usable.
    shaderBindings_.reset();
    uniformBuffer_.reset();
    uniformBuffer_ = std::move(newUniformBuffer);
    shaderBindings_ = std::move(newShaderBindings);
    uniformStride_ = newUniformStride;
    uniformCapacity_ = drawCapacity;
}

void PointCloudRenderer::ensureUniformCapacity(const std::size_t drawCount)
{
    if (drawCount <= uniformCapacity_) {
        return;
    }
    const std::size_t capacity =
        grownUniformDrawCapacity(uniformCapacity_, drawCount);
    createResourceBindings(capacity);
    ++uniformCapacityGrowthCount_;
}

void PointCloudRenderer::createPipeline(
    QRhiRenderPassDescriptor *renderPassDescriptor)
{
    pipeline_.reset();

    QString error;
    const QShader vertexShader =
        loadShaderResource(QStringLiteral(":/shaders/points.vert.qsb"), &error);
    if (!vertexShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    const QShader fragmentShader =
        loadShaderResource(QStringLiteral(":/shaders/points.frag.qsb"), &error);
    if (!fragmentShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings(
        {QRhiVertexInputBinding(static_cast<quint32>(sizeof(GpuPoint)))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::UShort4, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::UNormByte4, 8),
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UInt, 12),
    });

    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setName(QByteArrayLiteral("Point pipeline"));
    pipeline_->setTopology(QRhiGraphicsPipeline::Points);
    pipeline_->setDepthTest(true);
    pipeline_->setDepthWrite(true);
    pipeline_->setDepthOp(QRhiGraphicsPipeline::Less);
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, fragmentShader},
    });
    pipeline_->setVertexInputLayout(inputLayout);
    pipeline_->setShaderResourceBindings(shaderBindings_.get());
    pipelineRenderPass_ = renderPassDescriptor;
    pipeline_->setRenderPassDescriptor(pipelineRenderPass_);
    requireCreated(pipeline_->create(), "point graphics pipeline");
}

} // namespace pci
