#pragma once

#include "renderer/planning/FrameCamera.h"
#include "renderer/rhi/RhiResource.h"
#include "scene/SceneDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiShaderResourceBindings;

namespace pci {

enum class VectorPrimitive : std::size_t {
    Fill = 0,
    Line = 1,
    Marker = 2
};
enum class VectorDepthMode : std::size_t {
    Tested = 0,
    AlwaysOnTop = 1
};

[[nodiscard]] constexpr std::size_t
vectorPipelineIndex(const VectorPrimitive primitive,
                    const VectorDepthMode mode) noexcept
{
    return static_cast<std::size_t>(primitive) * 2U +
           static_cast<std::size_t>(mode);
}

[[nodiscard]] std::optional<Bounds3d>
vectorLayerCullBounds(const VectorLayerData &geometry,
                      const VectorLayerStyle &style,
                      const FrameCamera &frame) noexcept;

struct alignas(16) VectorLayerUniform {
    std::array<float, 16> mvp{};
    std::array<float, 4> fillColor{};
    std::array<float, 4> strokeColor{};
    std::array<float, 4> markerColor{};
    std::array<float, 2> viewportPixels{};
    float strokeHalfWidthPixels = 0.0F;
    float markerHalfSizePixels = 0.0F;
    float opacity = 1.0F;
    std::int32_t markerShape = 0;
    float featherPixels = 1.0F;
    float nearPlaneW = 0.0F;
};
static_assert(alignof(VectorLayerUniform) == 16);
static_assert(std::is_standard_layout_v<VectorLayerUniform>);
static_assert(offsetof(VectorLayerUniform, mvp) == 0);
static_assert(offsetof(VectorLayerUniform, fillColor) == 64);
static_assert(offsetof(VectorLayerUniform, strokeColor) == 80);
static_assert(offsetof(VectorLayerUniform, markerColor) == 96);
static_assert(offsetof(VectorLayerUniform, viewportPixels) == 112);
static_assert(offsetof(VectorLayerUniform, strokeHalfWidthPixels) == 120);
static_assert(offsetof(VectorLayerUniform, markerHalfSizePixels) == 124);
static_assert(offsetof(VectorLayerUniform, opacity) == 128);
static_assert(offsetof(VectorLayerUniform, markerShape) == 132);
static_assert(offsetof(VectorLayerUniform, featherPixels) == 136);
static_assert(offsetof(VectorLayerUniform, nearPlaneW) == 140);
static_assert(sizeof(VectorLayerUniform) == 144);

struct VectorLayerDraw {
    SceneLayerId layerId;
    VectorLayer layer;
    VectorLayerUniform uniform;
    std::uint32_t uniformIndex = 0;
};

[[nodiscard]] std::vector<std::byte>
stageVectorLayerUniforms(std::span<const VectorLayerDraw> draws,
                         std::size_t uniformStride);

class VectorLayerRenderer {
public:
    VectorLayerRenderer() = default;
    ~VectorLayerRenderer();

    VectorLayerRenderer(const VectorLayerRenderer &) = delete;
    VectorLayerRenderer &operator=(const VectorLayerRenderer &) = delete;

    void ensureResources(QRhi *rhi, QRhiRenderPassDescriptor *renderPass);
    // Keeps immutable GPU geometry resident for every layer still owned by the
    // document. Visibility and frustum culling must not evict geometry.
    void syncLayers(QRhiCommandBuffer *commandBuffer,
                    std::span<const SceneLayer> layers);
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        std::span<const VectorLayerDraw> draws);
    void releaseResources();
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept;
    void recordDraws(QRhiCommandBuffer *commandBuffer,
                     QRhiRenderTarget *renderTarget,
                     std::span<const VectorLayerDraw> draws);

private:
    struct GpuFillBatch {
        RhiResourcePtr<QRhiBuffer> vertices;
        RhiResourcePtr<QRhiBuffer> indices;
        std::uint32_t indexCount = 0;
        std::uint64_t bytes = 0;
    };
    struct GpuLayer {
        const VectorLayerData *data = nullptr;
        std::vector<GpuFillBatch> fills;
        RhiResourcePtr<QRhiBuffer> segments;
        RhiResourcePtr<QRhiBuffer> markers;
        std::uint32_t segmentCount = 0;
        std::uint32_t markerCount = 0;
        std::uint64_t bytes = 0;
    };

    void createResourceBindings(std::size_t drawCapacity);
    void ensureUniformCapacity(std::size_t drawCount);
    void createPipelines(QRhiRenderPassDescriptor *renderPass);
    void ensureLayer(QRhiCommandBuffer *commandBuffer,
                     const VectorLayer &layer);
    void destroyLayer(GpuLayer &layer) noexcept;
    void pruneLayers(std::span<const SceneLayer> layers);

    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    RhiResourcePtr<QRhiShaderResourceBindings> shaderBindings_;
    std::array<RhiResourcePtr<QRhiGraphicsPipeline>, 6> pipelines_{};
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
    std::uint32_t uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    std::unordered_map<SceneLayerId, GpuLayer> layers_;
    std::uint64_t gpuBytes_ = 0;
};

} // namespace pci
