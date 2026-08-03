#pragma once

#include "renderer/rhi/RhiResource.h"

#include <QSize>

#include <span>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace pci {

inline constexpr float eyeDomeLightingRadius = 1.0F;
inline constexpr float eyeDomeLightingStrength = 25.0F;

[[nodiscard]] float
eyeDomeLightingShade(float centerViewDepth,
                     std::span<const float> neighbourViewDepths,
                     float strength = eyeDomeLightingStrength) noexcept;

class EyeDomeLightingPass {
public:
    EyeDomeLightingPass() = default;
    ~EyeDomeLightingPass();

    EyeDomeLightingPass(const EyeDomeLightingPass &) = delete;
    EyeDomeLightingPass &operator=(const EyeDomeLightingPass &) = delete;

    [[nodiscard]] bool ensureResources(
        QRhi *rhi, QSize pixelSize, QRhiRenderPassDescriptor *outputRenderPass);
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool matchesPointTarget(QRhi *rhi,
                                          QSize pixelSize) const noexcept;
    [[nodiscard]] QRhiRenderTarget *pointRenderTarget() const noexcept;
    [[nodiscard]] QRhiRenderPassDescriptor *
    pointRenderPassDescriptor() const noexcept;

    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        float nearPlane,
                        float farPlane);
    void recordComposite(QRhiCommandBuffer *commandBuffer,
                         QRhiRenderTarget *outputRenderTarget);
    void releaseResources();

private:
    void createSceneTarget(QSize pixelSize);
    void createSampler();
    void createBindings();
    void createPipeline(QRhiRenderPassDescriptor *outputRenderPass);

    QRhi *rhi_ = nullptr;
    QSize pixelSize_;
    RhiResourcePtr<QRhiTexture> colorTexture_;
    RhiResourcePtr<QRhiTexture> depthTexture_;
    RhiResourcePtr<QRhiTextureRenderTarget> pointRenderTarget_;
    RhiResourcePtr<QRhiRenderPassDescriptor> pointRenderPass_;
    RhiResourcePtr<QRhiSampler> sampler_;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    RhiResourcePtr<QRhiShaderResourceBindings> bindings_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor *outputRenderPass_ = nullptr;
};

} // namespace pci
