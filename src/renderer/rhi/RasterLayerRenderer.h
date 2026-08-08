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
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace pci {

struct alignas(16) RasterLayerUniform {
    std::array<float, 16> mvp{};
    std::array<float, 4> uvRect{}; // minU, minV, maxU, maxV inside the gutter
    float opacity = 1.0F;
    std::array<float, 3> padding{};
};
static_assert(alignof(RasterLayerUniform) == 16);
static_assert(std::is_standard_layout_v<RasterLayerUniform>);
static_assert(offsetof(RasterLayerUniform, mvp) == 0);
static_assert(offsetof(RasterLayerUniform, uvRect) == 64);
static_assert(offsetof(RasterLayerUniform, opacity) == 80);
static_assert(sizeof(RasterLayerUniform) == 96);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct RasterLayerDraw {
    SceneLayerId layerId;
    RasterLayerUniform uniform;
    std::uint32_t uniformIndex = 0;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

[[nodiscard]] std::vector<std::byte>
stageRasterLayerUniforms(std::span<const RasterLayerDraw> draws,
                         std::size_t uniformStride);

// Maps the unit quad onto the layer's world footprint. Computed eye-relative
// in double precision before narrowing, matching the convention the point and
// vector renderers already use at large projected coordinates. Rotation and
// skew from the geotransform are carried by the two edge vectors, so the
// shader needs no second vertex attribute.
struct RasterQuadTransform {
    Vec3d origin; // the unit (0,0) corner, relative to the camera eye
    Vec3d edgeU;  // unit (1,0) minus unit (0,0)
    Vec3d edgeV;  // unit (0,1) minus unit (0,0)
};

[[nodiscard]] RasterQuadTransform
rasterLayerQuadTransform(const RasterLayerMetadata &metadata,
                         const RasterLayerStyle &style,
                         Vec3d eye) noexcept;

[[nodiscard]] std::optional<Bounds3d>
rasterLayerCullBounds(const RasterLayerMetadata &metadata,
                      const RasterLayerStyle &style,
                      const FrameCamera &frame) noexcept;

// Phase 1: one static texture per layer, sized to the finest backed level that
// fits the device limit or to one bounded decimating base read. Phase 2
// replaces this with tiled residency.
class RasterLayerRenderer {
public:
    RasterLayerRenderer() = default;
    ~RasterLayerRenderer();

    RasterLayerRenderer(const RasterLayerRenderer &) = delete;
    RasterLayerRenderer &operator=(const RasterLayerRenderer &) = delete;

    void ensureResources(QRhi *rhi, QRhiRenderPassDescriptor *renderPass);
    // Uploads textures for layers the document still owns and releases the
    // rest. Must run before beginPass(): QRhi resource updates are not legal
    // inside an active render pass.
    void syncLayers(QRhiCommandBuffer *commandBuffer,
                    std::span<const SceneLayer> layers,
                    const PointColorMapCatalogSnapshotPtr &colorMaps);
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        std::span<const RasterLayerDraw> draws);
    void recordDraws(QRhiCommandBuffer *commandBuffer,
                     QRhiRenderTarget *renderTarget,
                     std::span<const RasterLayerDraw> draws);
    void releaseResources();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept;
    [[nodiscard]] std::size_t residentLayerCount() const noexcept;
    // Layers admitted as metadata whose source needs tiled rendering, or whose
    // read failed. They are counted rather than retried every frame.
    [[nodiscard]] std::size_t unavailableLayerCount() const noexcept;

private:
    struct GpuLayer {
        RhiResourcePtr<QRhiTexture> texture;
        RhiResourcePtr<QRhiShaderResourceBindings> bindings;
        RasterSourceId sourceId;
        std::uint64_t renderGeneration = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t bytes = 0;
        bool nearest = false;
        bool unavailable = false;
    };

    void createUniformBuffer(std::size_t drawCapacity);
    void ensureUniformCapacity(std::size_t drawCount);
    void createPipeline(QRhiRenderPassDescriptor *renderPass);
    void ensureLayer(QRhiCommandBuffer *commandBuffer,
                     const RasterLayer &layer,
                     const PointColorMapCatalogSnapshotPtr &colorMaps);
    void destroyLayer(GpuLayer &layer) noexcept;
    void pruneLayers(std::span<const SceneLayer> layers);

    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiBuffer> vertexBuffer_;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    RhiResourcePtr<QRhiSampler> linearSampler_;
    RhiResourcePtr<QRhiSampler> nearestSampler_;
    RhiResourcePtr<QRhiShaderResourceBindings> pipelineBindings_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
    bool quadUploaded_ = false;
    std::uint32_t uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    std::unordered_map<SceneLayerId, GpuLayer> layers_;
    std::uint64_t gpuBytes_ = 0;
};

} // namespace pci
