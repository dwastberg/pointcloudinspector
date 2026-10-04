#pragma once

#include <pci/rendering/rhi/RhiResource.h>
#include <pci/rendering/rhi/UniformStaging.h>

#include <pci/color/PointColorMapCatalog.h>
#include <pci/pointcloud/PointBlock.h>

#include <QtCore/qtypes.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
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

struct ScalarNormalization {
    float offset = 0.0F;
    float step = 0.0F;
};

[[nodiscard]] ScalarNormalization
blockScalarNormalization(double blockOrigin,
                         double blockScale,
                         double layerMinimum,
                         double layerMaximum) noexcept;

struct alignas(16) BlockUniform {
    float mvp[16]{};
    // Preserve the established cross-backend uniform offsets. This slot no
    // longer carries absolute world coordinates.
    float reserved[4]{};
    float pointSize = 1.0F;
    std::int32_t colorSource = 0;
    std::int32_t reservedColorMap = 0;
    float scalarOffset = 0.0F;
    float scalarStep = 0.0F;
    std::int32_t idBase = 0;
    float padding[2]{};
    std::uint32_t classificationMask[8]{
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
        0xffffffffU,
    };
};
static_assert(std::is_standard_layout_v<BlockUniform>);
static_assert(offsetof(BlockUniform, mvp) == 0);
static_assert(offsetof(BlockUniform, reserved) == 64);
static_assert(offsetof(BlockUniform, pointSize) == 80);
static_assert(offsetof(BlockUniform, colorSource) == 84);
static_assert(offsetof(BlockUniform, reservedColorMap) == 88);
static_assert(offsetof(BlockUniform, scalarOffset) == 92);
static_assert(offsetof(BlockUniform, scalarStep) == 96);
static_assert(offsetof(BlockUniform, idBase) == 100);
static_assert(offsetof(BlockUniform, padding) == 104);
static_assert(offsetof(BlockUniform, classificationMask) == 112);
static_assert(sizeof(BlockUniform) == 144);

// BlockUniform deliberately mirrors a 16-byte-aligned GPU uniform block.
// MSVC reports the resulting padding in this CPU-side draw record even though
// the alignment is part of the data contract.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct BlockDraw {
    PointBlockPtr block;
    QRhiBuffer *buffer = nullptr;
    quint32 pointCount = 0;
    quint32 idBase = 0;
    // Index into the full-frame dynamic-uniform array. Pick candidate lists
    // may be reordered or sparse, so their vector index is not sufficient.
    quint32 uniformIndex = 0;
    BlockUniform uniform;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

[[nodiscard]] float
largestDrawPointSizePixels(std::span<const BlockDraw> draws) noexcept;

[[nodiscard]] std::vector<std::byte>
stageBlockUniforms(std::span<const BlockDraw> draws, std::size_t uniformStride);

class PointCloudRenderer {
public:
    explicit PointCloudRenderer(PointColorMapCatalogSnapshotPtr colorMaps = {});
    ~PointCloudRenderer();

    PointCloudRenderer(const PointCloudRenderer &) = delete;
    PointCloudRenderer &operator=(const PointCloudRenderer &) = delete;

    void ensureResources(QRhi *rhi,
                         QRhiRenderPassDescriptor *renderPassDescriptor);
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        const std::vector<BlockDraw> &draws);
    void recordDraws(QRhiCommandBuffer *commandBuffer,
                     QRhiRenderTarget *renderTarget,
                     const std::vector<BlockDraw> &draws);
    [[nodiscard]] QRhiShaderResourceBindings *shaderBindings() const noexcept;
    [[nodiscard]] quint32 uniformStride() const noexcept;
    [[nodiscard]] std::size_t uniformDrawCapacity() const noexcept;
    [[nodiscard]] std::uint64_t uniformCapacityGrowthCount() const noexcept;
    [[nodiscard]] std::uint64_t uniformUpdateOperationCount() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    void releaseResources();

private:
    void createColorMapResources();
    void createResourceBindings(std::size_t drawCapacity);
    void createPipeline(QRhiRenderPassDescriptor *renderPassDescriptor);
    void ensureUniformCapacity(std::size_t drawCount);

    QRhi *rhi_ = nullptr;
    RhiResourcePtr<QRhiBuffer> uniformBuffer_;
    quint32 uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    std::uint64_t uniformCapacityGrowthCount_ = 0;
    std::uint64_t uniformUpdateOperationCount_ = 0;
    std::vector<std::byte> uniformStaging_;
    RhiResourcePtr<QRhiTexture> colorMapTexture_;
    RhiResourcePtr<QRhiSampler> colorMapSampler_;
    bool colorMapUploadPending_ = false;
    PointColorMapCatalogSnapshotPtr colorMaps_;
    RhiResourcePtr<QRhiShaderResourceBindings> shaderBindings_;
    RhiResourcePtr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
};

} // namespace pci
