#include <pci/rendering/rhi/PointPicker.h>

#include <pci/pointcloud/GpuPoint.h>
#include <pci/rendering/rhi/ShaderLoader.h>

#include <QColor>
#include <QRect>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pci {

struct PointPicker::ReadbackState {
    QRhiReadbackResult result;
    Completion delivery;
    bool pending = true;
};

namespace {

void requireCreated(const bool created, const char *resource)
{
    if (!created) {
        throw std::runtime_error(QStringLiteral("Could not create QRhi %1")
                                     .arg(QString::fromUtf8(resource))
                                     .toStdString());
    }
}

} // namespace

PointPicker::~PointPicker()
{
    releaseResources();
}

std::optional<std::uint32_t> PointPicker::decodeNearestId(
    const QByteArray &data, const QSize size, const int radiusPixels)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return std::nullopt;
    }
    const qsizetype expectedBytes =
        static_cast<qsizetype>(size.width()) *
        static_cast<qsizetype>(size.height()) *
        static_cast<qsizetype>(sizeof(std::uint32_t));
    if (data.size() != expectedBytes) {
        return std::nullopt;
    }

    const double centerX = (static_cast<double>(size.width()) - 1.0) * 0.5;
    const double centerY = (static_cast<double>(size.height()) - 1.0) * 0.5;
    double nearestDistanceSquared = std::numeric_limits<double>::max();
    std::optional<std::uint32_t> nearest;

    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            std::uint32_t storedId = 0;
            const qsizetype offset = static_cast<qsizetype>(
                (y * size.width() + x) * sizeof(storedId));
            std::memcpy(&storedId, data.constData() + offset, sizeof(storedId));
            if (storedId == 0) {
                continue;
            }
            const double deltaX = static_cast<double>(x) - centerX;
            const double deltaY = static_cast<double>(y) - centerY;
            const double distanceSquared = deltaX * deltaX + deltaY * deltaY;
            if (radiusPixels >= 0 &&
                distanceSquared >
                    static_cast<double>(radiusPixels * radiusPixels)) {
                continue;
            }
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                nearest = storedId - 1U;
            }
        }
    }
    return nearest;
}

void PointPicker::ensureResources(QRhi *rhi,
                                  QRhiShaderResourceBindings *shaderBindings)
{
    if (!rhi || !shaderBindings) {
        return;
    }
    // The pipeline retains the binding layout, not this renderer-owned
    // binding object. record() receives the current layout-compatible object.
    if (rhi_ == rhi && pipeline_) {
        return;
    }

    releaseResources();
    const QSize pixelSize(pickTargetEdge, pickTargetEdge);
    const auto textureFlags =
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource;
    if (!rhi->isTextureFormatSupported(QRhiTexture::R32UI, textureFlags)) {
        throw std::runtime_error(
            "GPU device does not support R32UI picking targets");
    }

    rhi_ = rhi;

    idTexture_.reset(
        rhi_->newTexture(QRhiTexture::R32UI, pixelSize, 1, textureFlags));
    idTexture_->setName(QByteArrayLiteral("Point pick IDs"));
    requireCreated(idTexture_->create(), "point-pick ID texture");

    depthBuffer_.reset(
        rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize));
    depthBuffer_->setName(QByteArrayLiteral("Point pick depth"));
    requireCreated(depthBuffer_->create(), "point-pick depth buffer");

    const QRhiTextureRenderTargetDescription description(
        QRhiColorAttachment(idTexture_.get()), depthBuffer_.get());
    renderTarget_.reset(rhi_->newTextureRenderTarget(description));
    renderTarget_->setName(QByteArrayLiteral("Point pick target"));
    renderPass_.reset(renderTarget_->newCompatibleRenderPassDescriptor());
    renderTarget_->setRenderPassDescriptor(renderPass_.get());
    requireCreated(renderTarget_->create(), "point-pick render target");

    QString error;
    const QShader vertexShader =
        loadShaderResource(QStringLiteral(":/shaders/pick.vert.qsb"), &error);
    if (!vertexShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    const QShader fragmentShader =
        loadShaderResource(QStringLiteral(":/shaders/pick.frag.qsb"), &error);
    if (!fragmentShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings(
        {QRhiVertexInputBinding(static_cast<quint32>(sizeof(GpuPoint)))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::UShort4, 0),
    });

    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setName(QByteArrayLiteral("Point pick pipeline"));
    pipeline_->setTopology(QRhiGraphicsPipeline::Points);
    pipeline_->setDepthTest(true);
    pipeline_->setDepthWrite(true);
    pipeline_->setDepthOp(QRhiGraphicsPipeline::Less);
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, fragmentShader},
    });
    pipeline_->setVertexInputLayout(inputLayout);
    pipeline_->setShaderResourceBindings(shaderBindings);
    pipeline_->setRenderPassDescriptor(renderPass_.get());
    requireCreated(pipeline_->create(), "point-pick pipeline");
    expectedBindingLayout_ = shaderBindings->serializedLayoutDescription();
}

bool PointPicker::inFlight() const noexcept
{
    return readback_ && readback_->pending;
}

void PointPicker::record(QRhiCommandBuffer *commandBuffer,
                         QRhiShaderResourceBindings *currentShaderBindings,
                         const std::vector<BlockDraw> &draws,
                         const quint32 uniformStride,
                         const QPoint pixelPosition,
                         const QSize renderTargetPixelSize,
                         const int readbackRadiusPixels,
                         Completion completion)
{
    if (!commandBuffer || !currentShaderBindings || !pipeline_ || inFlight()) {
        return;
    }
    if (currentShaderBindings->serializedLayoutDescription() !=
        expectedBindingLayout_) {
        throw std::logic_error(
            "point picker received an incompatible resource-binding layout");
    }
    if (draws.empty()) {
        completion(std::nullopt);
        return;
    }

    // Translate the full-size viewport so the cursor pixel lands at the
    // centre of the fixed 64x64 target. QRhi viewports are bottom-left
    // based; the cursor position is top-left based.
    const auto half = static_cast<float>(pickTargetEdge) * 0.5F;
    const float viewportX = half - static_cast<float>(pixelPosition.x());
    const float viewportY =
        half - (static_cast<float>(renderTargetPixelSize.height()) -
                static_cast<float>(pixelPosition.y()));

#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkBegin(QByteArrayLiteral("Point picking"));
#endif
    commandBuffer->beginPass(
        renderTarget_.get(), QColor::fromRgba64(0, 0, 0, 0), {1.0F, 0});
    commandBuffer->setGraphicsPipeline(pipeline_.get());
    commandBuffer->setViewport(
        QRhiViewport(viewportX,
                     viewportY,
                     static_cast<float>(renderTargetPixelSize.width()),
                     static_cast<float>(renderTargetPixelSize.height())));
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const QRhiCommandBuffer::DynamicOffset offset(
            0, draws[i].uniformIndex * uniformStride);
        commandBuffer->setShaderResources(currentShaderBindings, 1, &offset);
        const QRhiCommandBuffer::VertexInput vertexInput(draws[i].buffer, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(draws[i].pointCount);
    }
    commandBuffer->endPass();
#if defined(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI) && !defined(NDEBUG)
    commandBuffer->debugMarkEnd();
#endif

    readback_ = std::make_shared<ReadbackState>();
    readback_->delivery = std::move(completion);
    readback_->result.completed =
        [weak = std::weak_ptr<ReadbackState>(readback_)] {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            state->pending = false;
            const auto result =
                decodeNearestId(state->result.data,
                                state->result.pixelSize,
                                (state->result.pixelSize.width() - 1) / 2);
            auto delivery = std::move(state->delivery);
            if (delivery) {
                delivery(result);
            }
        };
    QRhiReadbackDescription description(idTexture_.get());
    const int radius =
        std::clamp(readbackRadiusPixels, 0, pickTargetEdge / 2 - 1);
    const int center = pickTargetEdge / 2;
    description.setRect(QRect(
        center - radius, center - radius, radius * 2 + 1, radius * 2 + 1));
    RhiResourceUpdateBatchPtr updates(rhi_->nextResourceUpdateBatch());
    updates->readBackTexture(description, &readback_->result);
    commandBuffer->resourceUpdate(updates.release());
}

void PointPicker::releaseResources()
{
    if (readback_) {
        readback_->delivery = {};
        // Teardown only: QRhi still owns the result address until the submitted
        // readback completes. UI invalidation alone cannot release that
        // storage.
        if (readback_->pending && rhi_) {
            rhi_->finish();
        }
        readback_.reset();
    }
    pipeline_.reset();
    expectedBindingLayout_.clear();
    renderTarget_.reset();
    renderPass_.reset();
    depthBuffer_.reset();
    idTexture_.reset();
    rhi_ = nullptr;
}

} // namespace pci
