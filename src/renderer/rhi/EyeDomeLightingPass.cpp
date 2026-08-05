#include "renderer/rhi/EyeDomeLightingPass.h"

#include "renderer/rhi/ShaderLoader.h"

#include <QColor>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace pci {
namespace {

struct alignas(16) EyeDomeLightingUniforms {
    float inverseViewport[2]{};
    float radius = eyeDomeLightingRadius;
    float strength = eyeDomeLightingStrength;
    float nearPlane = 0.1F;
    float farPlane = 1.0F;
    float padding[2]{};
};
static_assert(offsetof(EyeDomeLightingUniforms, inverseViewport) == 0);
static_assert(offsetof(EyeDomeLightingUniforms, radius) == 8);
static_assert(offsetof(EyeDomeLightingUniforms, strength) == 12);
static_assert(offsetof(EyeDomeLightingUniforms, nearPlane) == 16);
static_assert(offsetof(EyeDomeLightingUniforms, farPlane) == 20);
static_assert(sizeof(EyeDomeLightingUniforms) == 32);

void requireCreated(const bool created, const char *resource)
{
    if (!created) {
        throw std::runtime_error("Could not create QRhi " +
                                 std::string(resource));
    }
}

} // namespace

float eyeDomeLightingShade(const float centerViewDepth,
                           const std::span<const float> neighbourViewDepths,
                           const float strength) noexcept
{
    if (!std::isfinite(centerViewDepth) || centerViewDepth <= 0.0F ||
        !std::isfinite(strength) || strength <= 0.0F ||
        neighbourViewDepths.empty()) {
        return 1.0F;
    }

    const float centerLogDepth = std::log2(centerViewDepth);
    float response = 0.0F;
    for (const float neighbourDepth : neighbourViewDepths) {
        if (!std::isfinite(neighbourDepth) || neighbourDepth <= 0.0F) {
            continue;
        }
        response += std::max(0.0F, centerLogDepth - std::log2(neighbourDepth));
    }
    response /= static_cast<float>(neighbourViewDepths.size());
    return std::clamp(std::exp(-strength * response), 0.0F, 1.0F);
}

EyeDomeLightingPass::~EyeDomeLightingPass()
{
    releaseResources();
}

bool EyeDomeLightingPass::ensureResources(
    QRhi *rhi,
    const QSize pixelSize,
    QRhiRenderPassDescriptor *outputRenderPass)
{
    if (!rhi || !outputRenderPass || pixelSize.isEmpty()) {
        return false;
    }
    if (!rhi->isTextureFormatSupported(QRhiTexture::D32F,
                                       QRhiTexture::RenderTarget)) {
        releaseResources();
        return false;
    }

    if (rhi_ != rhi || pixelSize_ != pixelSize) {
        releaseResources();
        rhi_ = rhi;
        pixelSize_ = pixelSize;
        createSceneTarget(pixelSize);
        createSampler();
        createBindings();
    }
    if (!pipeline_ || outputRenderPass_ != outputRenderPass) {
        createPipeline(outputRenderPass);
    }
    return available();
}

bool EyeDomeLightingPass::available() const noexcept
{
    return rhi_ && colorTexture_ && depthTexture_ && pointRenderTarget_ &&
           pointRenderPass_ && sampler_ && uniformBuffer_ && bindings_ &&
           pipeline_;
}

bool EyeDomeLightingPass::matchesPointTarget(
    QRhi *rhi, const QSize pixelSize) const noexcept
{
    return rhi_ == rhi && pixelSize_ == pixelSize && pointRenderTarget_ &&
           pointRenderPass_;
}

QRhiRenderTarget *EyeDomeLightingPass::pointRenderTarget() const noexcept
{
    return pointRenderTarget_.get();
}

QRhiRenderPassDescriptor *
EyeDomeLightingPass::pointRenderPassDescriptor() const noexcept
{
    return pointRenderPass_.get();
}

void EyeDomeLightingPass::updateUniforms(QRhiCommandBuffer *commandBuffer,
                                         const float nearPlane,
                                         const float farPlane)
{
    if (!available() || !commandBuffer || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane) || nearPlane <= 0.0F ||
        farPlane <= nearPlane) {
        throw std::logic_error("eye-dome lighting pass is not ready to update");
    }

    EyeDomeLightingUniforms uniforms;
    uniforms.inverseViewport[0] = 1.0F / static_cast<float>(pixelSize_.width());
    uniforms.inverseViewport[1] =
        1.0F / static_cast<float>(pixelSize_.height());
    uniforms.nearPlane = nearPlane;
    uniforms.farPlane = farPlane;

    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    updates->updateDynamicBuffer(uniformBuffer_.get(),
                                 0,
                                 static_cast<quint32>(sizeof(uniforms)),
                                 &uniforms);
    commandBuffer->resourceUpdate(updates.release());
}

void EyeDomeLightingPass::recordComposite(QRhiCommandBuffer *commandBuffer,
                                          QRhiRenderTarget *outputRenderTarget)
{
    if (!available() || !commandBuffer || !outputRenderTarget) {
        throw std::logic_error("eye-dome lighting pass is not ready to record");
    }
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkBegin(QByteArrayLiteral("Eye-dome lighting"));
#endif
    commandBuffer->setGraphicsPipeline(pipeline_.get());
    commandBuffer->setViewport(QRhiViewport(
        0,
        0,
        static_cast<float>(outputRenderTarget->pixelSize().width()),
        static_cast<float>(outputRenderTarget->pixelSize().height())));
    commandBuffer->setShaderResources(bindings_.get());
    commandBuffer->draw(3);
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkEnd();
#endif
}

void EyeDomeLightingPass::releaseResources()
{
    pipeline_.reset();
    outputRenderPass_ = nullptr;
    bindings_.reset();
    uniformBuffer_.reset();
    sampler_.reset();
    pointRenderTarget_.reset();
    pointRenderPass_.reset();
    depthTexture_.reset();
    colorTexture_.reset();
    pixelSize_ = {};
    rhi_ = nullptr;
}

void EyeDomeLightingPass::createSceneTarget(const QSize pixelSize)
{
    colorTexture_.reset(rhi_->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1, QRhiTexture::RenderTarget));
    colorTexture_->setName(QByteArrayLiteral("EDL point color"));
    requireCreated(colorTexture_->create(), "EDL color texture");

    depthTexture_.reset(rhi_->newTexture(
        QRhiTexture::D32F, pixelSize, 1, QRhiTexture::RenderTarget));
    depthTexture_->setName(QByteArrayLiteral("EDL point depth"));
    requireCreated(depthTexture_->create(), "EDL depth texture");

    const QRhiTextureRenderTargetDescription description(
        QRhiColorAttachment(colorTexture_.get()), depthTexture_.get());
    pointRenderTarget_.reset(rhi_->newTextureRenderTarget(description));
    pointRenderTarget_->setName(QByteArrayLiteral("EDL point target"));
    pointRenderPass_.reset(
        pointRenderTarget_->newCompatibleRenderPassDescriptor());
    pointRenderTarget_->setRenderPassDescriptor(pointRenderPass_.get());
    requireCreated(pointRenderTarget_->create(), "EDL point render target");
}

void EyeDomeLightingPass::createSampler()
{
    sampler_.reset(rhi_->newSampler(QRhiSampler::Nearest,
                                    QRhiSampler::Nearest,
                                    QRhiSampler::None,
                                    QRhiSampler::ClampToEdge,
                                    QRhiSampler::ClampToEdge));
    sampler_->setName(QByteArrayLiteral("EDL sampler"));
    requireCreated(sampler_->create(), "EDL sampler");
}

void EyeDomeLightingPass::createBindings()
{
    uniformBuffer_.reset(
        rhi_->newBuffer(QRhiBuffer::Dynamic,
                        QRhiBuffer::UniformBuffer,
                        static_cast<quint32>(sizeof(EyeDomeLightingUniforms))));
    uniformBuffer_->setName(QByteArrayLiteral("EDL uniforms"));
    requireCreated(uniformBuffer_->create(), "EDL uniform buffer");

    bindings_.reset(rhi_->newShaderResourceBindings());
    bindings_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, uniformBuffer_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::FragmentStage,
            colorTexture_.get(),
            sampler_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2,
            QRhiShaderResourceBinding::FragmentStage,
            depthTexture_.get(),
            sampler_.get()),
    });
    requireCreated(bindings_->create(), "EDL resource bindings");
}

void EyeDomeLightingPass::createPipeline(
    QRhiRenderPassDescriptor *outputRenderPass)
{
    pipeline_.reset();

    QString error;
    const QShader vertexShader =
        loadShaderResource(QStringLiteral(":/shaders/edl.vert.qsb"), &error);
    if (!vertexShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    const QShader fragmentShader =
        loadShaderResource(QStringLiteral(":/shaders/edl.frag.qsb"), &error);
    if (!fragmentShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }

    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setName(QByteArrayLiteral("Eye-dome lighting pipeline"));
    pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    pipeline_->setDepthTest(true);
    pipeline_->setDepthOp(QRhiGraphicsPipeline::Always);
    pipeline_->setDepthWrite(true);
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, fragmentShader},
    });
    pipeline_->setShaderResourceBindings(bindings_.get());
    outputRenderPass_ = outputRenderPass;
    pipeline_->setRenderPassDescriptor(outputRenderPass_);
    requireCreated(pipeline_->create(), "eye-dome lighting pipeline");
}

} // namespace pci
