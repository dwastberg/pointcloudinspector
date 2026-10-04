#pragma once

#include <pci/raster/RasterLayer.h>
#include <pci/rendering/planning/FrameCamera.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace pci {

// Refine when a level's texel projects to more than this many screen pixels;
// coarsen when the next coarser level's texel projects to fewer than the lower
// threshold. The coarsen test is made against the *candidate* level rather
// than the current one, which is what keeps the pair stable for arbitrary
// overview ratios instead of only for a 2x pyramid.
inline constexpr double rasterRefineTexelPixels = 1.25;
inline constexpr double rasterCoarsenTexelPixels = 0.8;

// Borrowed immutable inputs for one planning call. Keeping source ownership,
// layer identity, and document/runtime objects outside this view makes it
// impossible for LOD selection to enqueue work or read pixels.
struct RasterLodLayerView {
    const RasterLayerMetadata &metadata;
    const RasterLayerStyle &style;
    RasterElevationStatus elevationStatus;
    const std::optional<RasterElevationRange> &exactElevationRange;
};

struct RasterLodPlanInput {
    RasterLodLayerView layer;
    FrameCamera camera;

    RasterLodPlanInput(RasterLodLayerView layerView, FrameCamera frameCamera)
        : layer(layerView)
        , camera(frameCamera)
    {
    }

    // Sorted by level then y then x, so a region's prior level is found by
    // geometric overlap rather than key equality. Key equality would miss on
    // most frames during a zoom, silently disabling hysteresis.
    std::span<const RasterTileKey> previousSelection;
    std::function<bool(RasterTileKey)> cpuResident;
    std::function<bool(RasterTileKey)> gpuResident;
    // A failed generated tile is bypassed in favour of finer descendants.
    // This prevents a permanently failed coverage tile from blocking the
    // source-backed data beneath it.
    std::function<bool(RasterTileKey)> unavailable;
    // Hard planner ceiling. The effective cap is
    // min(maximumSelectedTiles, gpuCapacityTiles): the two are independent
    // limits, a fixed ceiling and a budget-derived residency ceiling, and the
    // planner always honors the smaller.
    std::size_t maximumSelectedTiles = 512;
    std::size_t gpuCapacityTiles = 512;
};

struct RasterLodPlan {
    std::vector<RasterTileKey> selected; // target detail
    std::vector<RasterTileKey> draw;     // resident target or ancestors
    std::vector<RasterTileKey> requests; // priority order
    // CPU-resident but GPU-missing target/fallback tiles. Keeping this
    // separate from source reads lets a recreated QRhi repopulate residency
    // without rereading GDAL.
    std::vector<RasterTileKey> decodedUploads;
    std::vector<RasterTileKey> protectedTiles;
    // True only when the selected cells do not cover the whole visible raster
    // footprint. Capacity-limited refinement can still have complete coverage
    // because its coarser parent remains selected.
    bool coverageIncomplete = false;
    bool capacityLimited = false;
};

[[nodiscard]] RasterLodPlan planRasterTiles(const RasterLodPlanInput &input);

// The visible region of the raster plane in level-0 pixel coordinates, derived
// by clipping the layer's finite quad against the frustum and mapping the
// result back through the inverse geotransform. Planning cost is therefore
// proportional to visible screen coverage rather than to catalog extent.
[[nodiscard]] std::vector<std::array<double, 2>>
rasterVisiblePixelPolygon(const RasterLodLayerView &layer,
                          const FrameCamera &camera);

// Screen size in pixels of one source texel at the given level, measured at a
// world position. Used for both the refine and coarsen thresholds.
[[nodiscard]] double rasterProjectedTexelPixels(const RasterLayerMetadata &,
                                                const RasterLevel &level,
                                                Vec3d worldPosition,
                                                const FrameCamera &camera);

} // namespace pci
