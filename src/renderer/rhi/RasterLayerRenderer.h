#pragma once

#include "raster/RasterTileCache.h"
#include "renderer/planning/FrameCamera.h"
#include "renderer/rhi/RhiResource.h"
#include "scene/SceneDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
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

inline constexpr std::uint32_t rasterSurfaceGridVerticesPerSide = 65;
inline constexpr std::uint32_t rasterSurfaceGridCellsPerSide =
    rasterSurfaceGridVerticesPerSide - 1;
inline constexpr std::uint32_t rasterSurfaceGridIndexCount =
    rasterSurfaceGridCellsPerSide * rasterSurfaceGridCellsPerSide * 6;
static_assert(rasterSurfaceGridVerticesPerSide *
                  rasterSurfaceGridVerticesPerSide <=
              std::numeric_limits<std::uint16_t>::max());
static_assert(rasterTileGutter >= 1,
              "the fragment central-difference normal reads one texel "
              "outside the sampled position");

// Shared std140 ABI for raster_surface.vert, raster_surface.frag, and C++.
// The absolute elevation anchor is deliberately folded into origin.z in
// double precision instead of being narrowed into this block.
struct alignas(16) RasterSurfaceUniform {
    std::array<float, 16> viewProjection{};
    std::array<float, 4> origin{};
    std::array<float, 4> edgeU{};
    std::array<float, 4> edgeV{};
    std::array<float, 4> uvRect{};
    std::array<float, 4> tileTexels{};
    std::array<float, 4> heightParams{};
    std::array<float, 4> shadingParams{};
};
static_assert(alignof(RasterSurfaceUniform) == 16);
static_assert(std::is_standard_layout_v<RasterSurfaceUniform>);
static_assert(offsetof(RasterSurfaceUniform, viewProjection) == 0);
static_assert(offsetof(RasterSurfaceUniform, origin) == 64);
static_assert(offsetof(RasterSurfaceUniform, edgeU) == 80);
static_assert(offsetof(RasterSurfaceUniform, edgeV) == 96);
static_assert(offsetof(RasterSurfaceUniform, uvRect) == 112);
static_assert(offsetof(RasterSurfaceUniform, tileTexels) == 128);
static_assert(offsetof(RasterSurfaceUniform, heightParams) == 144);
static_assert(offsetof(RasterSurfaceUniform, shadingParams) == 160);
static_assert(sizeof(RasterSurfaceUniform) == 176);

enum class RasterDrawMode : std::uint8_t {
    Flat,
    Surface,
};

enum class RasterSurfaceDrawRole : std::uint8_t {
    Detail,
    Fallback,
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct RasterLayerDraw {
    SceneLayerId layerId;
    RasterCacheKey tileKey;
    RasterLayerUniform uniform;
    std::uint32_t uniformIndex = 0;
    RasterSurfaceUniform surfaceUniform;
    std::uint32_t surfaceUniformIndex = 0;
    RasterDrawMode mode = RasterDrawMode::Flat;
    RasterSurfaceDrawRole surfaceRole = RasterSurfaceDrawRole::Detail;
    bool transparent = false;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

[[nodiscard]] std::vector<std::byte>
stageRasterLayerUniforms(std::span<const RasterLayerDraw> draws,
                         std::size_t uniformStride);

[[nodiscard]] std::vector<std::byte>
stageRasterSurfaceUniforms(std::span<const RasterLayerDraw> draws,
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
    [[nodiscard]] std::optional<RasterElevationRange>
    tileElevationResidualRange(const RasterCacheKey &key) const noexcept;
    // Drops GPU tiles for layers the document no longer owns.
    void retainLayers(std::span<const SceneLayerId> layerIds);
    void releaseSource(RasterSourceId sourceId);
    void setGpuByteBudget(std::uint64_t bytes,
                          std::span<const RasterCacheKey> protectedKeys);
    [[nodiscard]] std::size_t residentTileCount() const noexcept;
    // Tiles the GPU budget can hold, which the planner takes as one of its two
    // independent capacity ceilings.
    [[nodiscard]] std::size_t tileCapacity() const noexcept;
    [[nodiscard]] std::size_t
    tileCapacity(RasterTilePayloadProfile profile) const noexcept;
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        std::span<const RasterLayerDraw> draws);
    void recordDraws(QRhiCommandBuffer *commandBuffer,
                     QRhiRenderTarget *renderTarget,
                     std::span<const RasterLayerDraw> draws);
    void releaseResources();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool surfaceSupported() const noexcept;
    [[nodiscard]] const std::string &surfaceCapabilityReason() const noexcept;
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept;
    [[nodiscard]] std::uint64_t heightGpuBytes() const noexcept;
    [[nodiscard]] std::uint64_t gpuByteBudget() const noexcept;
    [[nodiscard]] std::uint64_t peakGpuBytes() const noexcept;

private:
    struct GpuTile {
        RhiResourcePtr<QRhiTexture> texture;
        RhiResourcePtr<QRhiShaderResourceBindings> bindings;
        RhiResourcePtr<QRhiTexture> elevationTexture;
        RhiResourcePtr<QRhiShaderResourceBindings> surfaceBindings;
        SceneLayerId layerId;
        std::uint64_t bytes = 0;
        std::uint64_t heightBytes = 0;
        std::uint64_t lastUsedFrame = 0;
        std::uint16_t validWidth = 0;
        std::uint16_t validHeight = 0;
        bool hasTranslucentAlpha = false;
        bool nearest = false;
        float elevationMinimum = 0.0F;
        float elevationMaximum = 0.0F;
        bool hasValidElevation = false;
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
    void createSurfaceResources(QRhiRenderPassDescriptor *renderPass);
    void releaseSurfaceResources() noexcept;
    void createSurfaceUniformBuffer(std::size_t drawCapacity);
    void ensureSurfaceUniformCapacity(std::size_t drawCount);
    void rebuildSurfaceBindings();
    void createSurfacePipelines(QRhiRenderPassDescriptor *renderPass);
    [[nodiscard]] bool makeRoom(std::uint64_t incoming, EvictionPlan &plan);
    void destroyTile(GpuTile &tile) noexcept;

    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    RhiResourcePtr<QRhiSampler> linearSampler_;
    RhiResourcePtr<QRhiSampler> nearestSampler_;
    RhiResourcePtr<QRhiShaderResourceBindings> pipelineBindings_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    RhiResourcePtr<QRhiBuffer> surfaceUniformBuffer_;
    RhiResourcePtr<QRhiBuffer> surfaceIndexBuffer_;
    RhiResourcePtr<QRhiShaderResourceBindings> surfacePipelineBindings_;
    std::array<RhiResourcePtr<QRhiGraphicsPipeline>, 4> surfacePipelines_;
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
    QRhiRenderPassDescriptor *surfacePipelineRenderPass_ = nullptr;
    std::uint32_t uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    std::uint32_t surfaceUniformStride_ = 0;
    std::size_t surfaceUniformCapacity_ = 0;
    bool surfaceIndexUploaded_ = false;
    bool surfaceSupported_ = false;
    std::string surfaceCapabilityReason_;
    std::unordered_map<RasterCacheKey, GpuTile> tiles_;
    std::uint64_t gpuBytes_ = 0;
    std::uint64_t peakGpuBytes_ = 0;
    std::uint64_t gpuByteBudget_ = 0;
    std::uint64_t frameCounter_ = 0;
};

} // namespace pci
