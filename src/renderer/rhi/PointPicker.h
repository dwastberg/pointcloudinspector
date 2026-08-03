#pragma once

#include "renderer/rhi/PointCloudRenderer.h"
#include "renderer/rhi/RhiResource.h"

#include <QByteArray>
#include <QPoint>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiReadbackResult;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace pci {

class PointPicker {
public:
    static constexpr int pickTargetEdge = 64;

    using Completion = std::function<void(std::optional<std::uint32_t>)>;

    PointPicker() = default;
    ~PointPicker();

    PointPicker(const PointPicker &) = delete;
    PointPicker &operator=(const PointPicker &) = delete;

    [[nodiscard]] static std::optional<std::uint32_t>
    decodeNearestId(const QByteArray &data, QSize size, int radiusPixels = -1);

    void ensureResources(QRhi *rhi, QRhiShaderResourceBindings *shaderBindings);
    [[nodiscard]] bool inFlight() const noexcept;
    void record(QRhiCommandBuffer *commandBuffer,
                QRhiShaderResourceBindings *currentShaderBindings,
                const std::vector<BlockDraw> &draws,
                quint32 uniformStride,
                QPoint pixelPosition,
                QSize renderTargetPixelSize,
                int readbackRadiusPixels,
                Completion completion);
    void releaseResources();

private:
    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiTexture> idTexture_;
    RhiResourcePtr<QRhiRenderBuffer> depthBuffer_;
    RhiResourcePtr<QRhiTextureRenderTarget> renderTarget_;
    RhiResourcePtr<QRhiRenderPassDescriptor> renderPass_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    QVector<quint32> expectedBindingLayout_;
    std::unique_ptr<QRhiReadbackResult> readback_;
    bool inFlight_ = false;
};

} // namespace pci
