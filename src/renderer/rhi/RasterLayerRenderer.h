#pragma once

#include "raster/RasterTileCache.h"
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
#include <unordered_set>
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
    std::array<float, 4> clipTopLeft{};
    std::array<float, 4> clipTopRight{};
    std::array<float, 4> clipBottomLeft{};
    std::array<float, 4> clipBottomRight{};
    std::array<float, 4> uvRect{}; // minU, minV, maxU, maxV inside the gutter
    float opacity = 1.0F;
    std::array<float, 3> padding{};
};
static_assert(alignof(RasterLayerUniform) == 16);
static_assert(std::is_standard_layout_v<RasterLayerUniform>);
static_assert(offsetof(RasterLayerUniform, clipTopLeft) == 0);
static_assert(offsetof(RasterLayerUniform, clipTopRight) == 16);
static_assert(offsetof(RasterLayerUniform, clipBottomLeft) == 32);
static_assert(offsetof(RasterLayerUniform, clipBottomRight) == 48);
static_assert(offsetof(RasterLayerUniform, uvRect) == 64);
static_assert(offsetof(RasterLayerUniform, opacity) == 80);
static_assert(sizeof(RasterLayerUniform) == 96);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct RasterLayerDraw {
    SceneLayerId layerId;
    RasterCacheKey tileKey;
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
// skew from the geotransform are carried by the two edge vectors. The viewport
// projects the resulting four corners before narrowing them for the shader.
struct RasterQuadTransform {
    Vec3d origin; // the unit (0,0) corner, relative to the camera eye
    Vec3d edgeU;  // unit (1,0) minus unit (0,0)
    Vec3d edgeV;  // unit (0,1) minus unit (0,0)
};

[[nodiscard]] RasterQuadTransform
rasterLayerQuadTransform(const RasterLayerMetadata &metadata,
                         const RasterLayerStyle &style,
                         Vec3d eye) noexcept;

// The same mapping for one tile, built from the tile's base-pixel rect so
// adjacent tiles and adjacent levels share an exact edge. The last row and
// column snap to the base dimensions, which is what keeps the outer boundary
// free of a floating-point gap.
[[nodiscard]] RasterQuadTransform
rasterTileQuadTransform(const RasterLayerMetadata &metadata,
                        const RasterLayerStyle &style,
                        RasterTileKey key,
                        Vec3d eye) noexcept;

// UVs cover only the tile's replicated-gutter interior, so the sampler never
// reaches a neighbouring tile's texels.
[[nodiscard]] std::array<float, 4>
rasterTileUvRect(std::uint16_t validWidth, std::uint16_t validHeight) noexcept;

// A decoded tile awaiting GPU residency. The pixels stay owned by the decoded
// cache until QRhi has consumed the update.
struct RasterPendingUpload {
    RasterCacheKey key;
    SceneLayerId layerId;
    const RasterTileData *tile = nullptr;
    bool nearest = false;
};

[[nodiscard]] std::optional<Bounds3d>
rasterLayerCullBounds(const RasterLayerMetadata &metadata,
                      const RasterLayerStyle &style,
                      const FrameCamera &frame) noexcept;

// Phase 1: one static texture per layer, sized to the finest backed level that
// fits the device limit or to one bounded decimating base read. Phase 2
// replaces this with tiled residency.
class RasterLayerRenderer {
public:
    explicit RasterLayerRenderer(std::uint64_t gpuByteBudget = 256ULL * 1024 *
                                                               1024);
    ~RasterLayerRenderer();

    RasterLayerRenderer(const RasterLayerRenderer &) = delete;
    RasterLayerRenderer &operator=(const RasterLayerRenderer &) = delete;

    void ensureResources(QRhi *rhi, QRhiRenderPassDescriptor *renderPass);
    // Uploads textures for layers the document still owns and releases the
    // rest. Must run before beginPass(): QRhi resource updates are not legal
    // inside an active render pass.
    // Creates textures for newly decoded tiles, stopping once the frame's
    // upload budget is spent so a large refinement is spread over frames
    // rather than stalling one. Returns the number uploaded.
    std::size_t uploadPending(QRhiCommandBuffer *commandBuffer,
                              std::span<const RasterPendingUpload> pending,
                              std::span<const RasterCacheKey> protectedKeys,
                              std::uint64_t frameByteBudget);
    [[nodiscard]] bool gpuResident(const RasterCacheKey &key) const noexcept;
    // Drops GPU tiles for layers the document no longer owns.
    void retainLayers(std::span<const SceneLayerId> layerIds);
    void releaseSource(RasterSourceId sourceId);
    void setGpuByteBudget(std::uint64_t bytes,
                          std::span<const RasterCacheKey> protectedKeys);
    [[nodiscard]] std::size_t residentTileCount() const noexcept;
    // Tiles the GPU budget can hold, which the planner takes as one of its two
    // independent capacity ceilings.
    [[nodiscard]] std::size_t tileCapacity() const noexcept;
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        std::span<const RasterLayerDraw> draws);
    void recordDraws(QRhiCommandBuffer *commandBuffer,
                     QRhiRenderTarget *renderTarget,
                     std::span<const RasterLayerDraw> draws);
    void releaseResources();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept;
    [[nodiscard]] std::uint64_t gpuByteBudget() const noexcept;
    [[nodiscard]] std::uint64_t peakGpuBytes() const noexcept;

private:
    struct GpuTile {
        RhiResourcePtr<QRhiTexture> texture;
        RhiResourcePtr<QRhiShaderResourceBindings> bindings;
        SceneLayerId layerId;
        std::uint64_t bytes = 0;
        std::uint64_t lastUsedFrame = 0;
        std::uint16_t validWidth = 0;
        std::uint16_t validHeight = 0;
    };

    // Eviction candidates in least-recently-used order, computed once per
    // upload pass. Rebuilding it per evicted tile made a full cache cost a
    // scan of every resident tile for every admission.
    struct EvictionPlan {
        std::vector<RasterCacheKey> order;
        std::size_t next = 0;
    };
    [[nodiscard]] EvictionPlan buildEvictionPlan(
        const std::unordered_set<RasterCacheKey> &protectedKeys) const;

    void createUniformBuffer(std::size_t drawCapacity);
    void ensureUniformCapacity(std::size_t drawCount);
    void createPipeline(QRhiRenderPassDescriptor *renderPass);
    [[nodiscard]] bool makeRoom(std::uint64_t incoming, EvictionPlan &plan);
    void destroyTile(GpuTile &tile) noexcept;

    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    RhiResourcePtr<QRhiSampler> linearSampler_;
    RhiResourcePtr<QRhiSampler> nearestSampler_;
    RhiResourcePtr<QRhiShaderResourceBindings> pipelineBindings_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
    std::uint32_t uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    std::unordered_map<RasterCacheKey, GpuTile> tiles_;
    std::uint64_t gpuBytes_ = 0;
    std::uint64_t peakGpuBytes_ = 0;
    std::uint64_t gpuByteBudget_ = 0;
    std::uint64_t frameCounter_ = 0;
};

} // namespace pci
