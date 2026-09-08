#include "raster/RasterLayer.h"

#include "foundation/Hash.h"

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

// The determinant is compared against the square of the largest linear
// coefficient rather than against machine epsilon, so that a metre-scale and a
// degree-scale transform are judged by the same relative criterion.
constexpr double affineDeterminantTolerance = 1.0e-12;

// A geographic raster wider than this in longitude has wrapped rather than
// genuinely covering that span.
constexpr double geographicWrapDegrees = 180.0;

[[nodiscard]] double linearScaleOf(const std::array<double, 6> &g) noexcept
{
    return std::max(
        {std::abs(g[1]), std::abs(g[2]), std::abs(g[4]), std::abs(g[5])});
}

[[nodiscard]] double affineDeterminant(const std::array<double, 6> &g) noexcept
{
    return g[1] * g[5] - g[2] * g[4];
}

} // namespace

bool rasterAffineInvertible(const std::array<double, 6> &geoTransform) noexcept
{
    if (!std::isfinite(geoTransform[0]) || !std::isfinite(geoTransform[3])) {
        return false;
    }
    const double scale = linearScaleOf(geoTransform);
    if (!std::isfinite(scale) || scale <= 0.0) {
        return false;
    }
    const double determinant = affineDeterminant(geoTransform);
    if (!std::isfinite(determinant)) {
        return false;
    }
    return std::abs(determinant) > scale * scale * affineDeterminantTolerance;
}

Vec3d rasterPixelToWorld(const std::array<double, 6> &geoTransform,
                         const double pixel,
                         const double line) noexcept
{
    return Vec3d{
        geoTransform[0] + pixel * geoTransform[1] + line * geoTransform[2],
        geoTransform[3] + pixel * geoTransform[4] + line * geoTransform[5],
        0.0,
    };
}

std::optional<RasterPixelCoordinate>
rasterWorldToPixel(const std::array<double, 6> &geoTransform,
                   const double worldX,
                   const double worldY) noexcept
{
    if (!rasterAffineInvertible(geoTransform)) {
        return std::nullopt;
    }
    const double determinant = affineDeterminant(geoTransform);
    const double offsetX = worldX - geoTransform[0];
    const double offsetY = worldY - geoTransform[3];
    const RasterPixelCoordinate coordinate{
        .pixel = (offsetX * geoTransform[5] - offsetY * geoTransform[2]) /
                 determinant,
        .line = (offsetY * geoTransform[1] - offsetX * geoTransform[4]) /
                determinant,
    };
    if (!std::isfinite(coordinate.pixel) || !std::isfinite(coordinate.line)) {
        return std::nullopt;
    }
    return coordinate;
}

std::array<Vec3d, 4>
rasterCornerPoints(const std::array<double, 6> &geoTransform,
                   const std::uint32_t width,
                   const std::uint32_t height) noexcept
{
    const auto right = static_cast<double>(width);
    const auto bottom = static_cast<double>(height);
    return {
        rasterPixelToWorld(geoTransform, 0.0, 0.0),
        rasterPixelToWorld(geoTransform, right, 0.0),
        rasterPixelToWorld(geoTransform, right, bottom),
        rasterPixelToWorld(geoTransform, 0.0, bottom),
    };
}

std::optional<Bounds3d>
rasterPixelEdgeBounds(const std::array<double, 6> &geoTransform,
                      const std::uint32_t width,
                      const std::uint32_t height) noexcept
{
    if (width == 0 || height == 0 || !rasterAffineInvertible(geoTransform)) {
        return std::nullopt;
    }
    const std::array<Vec3d, 4> corners =
        rasterCornerPoints(geoTransform, width, height);

    Bounds3d bounds;
    bounds.minimum = {corners[0].x, corners[0].y, 0.0};
    bounds.maximum = bounds.minimum;
    for (const Vec3d corner : corners) {
        if (!isFinite(corner)) {
            return std::nullopt;
        }
        bounds.minimum[0] = std::min(bounds.minimum[0], corner.x);
        bounds.minimum[1] = std::min(bounds.minimum[1], corner.y);
        bounds.maximum[0] = std::max(bounds.maximum[0], corner.x);
        bounds.maximum[1] = std::max(bounds.maximum[1], corner.y);
    }
    return bounds;
}

bool rasterCrossesAntimeridian(const Bounds3d &bounds,
                               const bool geographicCrs) noexcept
{
    if (!geographicCrs) {
        return false;
    }
    const double extent = bounds.maximum[0] - bounds.minimum[0];
    return std::isfinite(extent) && extent > geographicWrapDegrees;
}

bool rasterLevelTableValid(const std::span<const RasterLevel> levels,
                           const std::uint32_t baseWidth,
                           const std::uint32_t baseHeight) noexcept
{
    if (levels.empty() || baseWidth == 0 || baseHeight == 0) {
        return false;
    }
    if (levels.front().width != baseWidth ||
        levels.front().height != baseHeight ||
        levels.front().kind != RasterLevelKind::Backed) {
        return false;
    }
    bool generatedSeen = false;
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const RasterLevel &level = levels[index];
        if (level.width == 0 || level.height == 0) {
            return false;
        }
        if (level.channelCount == 0 || level.channelCount > 4) {
            return false;
        }
        if (!std::isfinite(level.basePixelsPerTexelX) ||
            !std::isfinite(level.basePixelsPerTexelY) ||
            level.basePixelsPerTexelX <= 0.0 ||
            level.basePixelsPerTexelY <= 0.0) {
            return false;
        }
        if (level.kind == RasterLevelKind::GeneratedCoverage) {
            generatedSeen = true;
        } else if (generatedSeen) {
            return false;
        }
        if (index == 0) {
            continue;
        }
        const RasterLevel &finer = levels[index - 1];
        if (level.width > finer.width || level.height > finer.height) {
            return false;
        }
        if (level.width == finer.width && level.height == finer.height) {
            return false;
        }
    }
    return true;
}

std::uint32_t rasterLevelTileCountX(const RasterLevel &level) noexcept
{
    if (level.width == 0) {
        return 0;
    }
    return (level.width + rasterTilePixels - 1) / rasterTilePixels;
}

std::uint32_t rasterLevelTileCountY(const RasterLevel &level) noexcept
{
    if (level.height == 0) {
        return 0;
    }
    return (level.height + rasterTilePixels - 1) / rasterTilePixels;
}

RasterTileExtent rasterTileValidExtent(const RasterLevel &level,
                                       const RasterTileKey key) noexcept
{
    if (key.x >= rasterLevelTileCountX(level) ||
        key.y >= rasterLevelTileCountY(level)) {
        return {};
    }
    const std::uint32_t originX = key.x * rasterTilePixels;
    const std::uint32_t originY = key.y * rasterTilePixels;
    return RasterTileExtent{
        .width = std::min(rasterTilePixels, level.width - originX),
        .height = std::min(rasterTilePixels, level.height - originY),
    };
}

bool RasterBasePixelRect::overlaps(
    const RasterBasePixelRect &other) const noexcept
{
    return minimumPixel < other.maximumPixel &&
           other.minimumPixel < maximumPixel &&
           minimumLine < other.maximumLine && other.minimumLine < maximumLine;
}

RasterBasePixelRect
rasterTileBasePixelRect(const RasterLevel &level,
                        const RasterTileKey key,
                        const std::uint32_t baseWidth,
                        const std::uint32_t baseHeight) noexcept
{
    const RasterTileExtent extent = rasterTileValidExtent(level, key);
    if (extent.width == 0 || extent.height == 0) {
        return {};
    }
    const auto originX = static_cast<double>(key.x) * rasterTilePixels;
    const auto originY = static_cast<double>(key.y) * rasterTilePixels;

    RasterBasePixelRect rect;
    rect.minimumPixel = originX * level.basePixelsPerTexelX;
    rect.minimumLine = originY * level.basePixelsPerTexelY;
    rect.maximumPixel = (originX + extent.width) * level.basePixelsPerTexelX;
    rect.maximumLine = (originY + extent.height) * level.basePixelsPerTexelY;

    // The outer edge is snapped to the base dimensions so that adjacent tiles
    // and adjacent levels share an identical boundary rather than differing by
    // an accumulated ratio rounding error.
    if (key.x + 1 == rasterLevelTileCountX(level)) {
        rect.maximumPixel = static_cast<double>(baseWidth);
    }
    if (key.y + 1 == rasterLevelTileCountY(level)) {
        rect.maximumLine = static_cast<double>(baseHeight);
    }
    rect.minimumPixel =
        std::min(rect.minimumPixel, static_cast<double>(baseWidth));
    rect.minimumLine =
        std::min(rect.minimumLine, static_cast<double>(baseHeight));
    return rect;
}

bool rasterRequiresTiledRendering(const RasterLayerMetadata &metadata) noexcept
{
    if (metadata.levels.empty()) {
        return true;
    }
    const auto coarsestBacked = std::ranges::find_last_if(
        metadata.levels, [](const RasterLevel &level) {
            return level.kind == RasterLevelKind::Backed;
        });
    if (coarsestBacked.empty()) {
        return true;
    }
    const RasterLevel &coarsest = *coarsestBacked.begin();
    if (coarsest.width <= rasterOverviewCoverageLimitPixels &&
        coarsest.height <= rasterOverviewCoverageLimitPixels) {
        return false;
    }
    // The coarsest level still needs many tiles to cover the view. That is
    // tolerable while the base itself is small, and a missing-overview problem
    // once it is not.
    const auto basePixels =
        static_cast<std::uint64_t>(metadata.width) * metadata.height;
    return basePixels > rasterBoundedBaseReadPixels;
}

void appendGeneratedRasterCoverageLevels(std::vector<RasterLevel> &levels,
                                         const std::uint32_t baseWidth,
                                         const std::uint32_t baseHeight)
{
    if (levels.empty() || baseWidth == 0 || baseHeight == 0 ||
        std::ranges::any_of(levels, [](const RasterLevel &level) {
            return level.kind == RasterLevelKind::GeneratedCoverage;
        })) {
        return;
    }

    const auto backed =
        std::ranges::find_last_if(levels, [](const RasterLevel &level) {
            return level.kind == RasterLevelKind::Backed;
        });
    if (backed.empty()) {
        return;
    }

    RasterLevel previous = *backed.begin();
    while (previous.width > rasterTilePixels ||
           previous.height > rasterTilePixels) {
        RasterLevel generated = previous;
        generated.width = std::max<std::uint32_t>(1, (previous.width + 1) / 2);
        generated.height =
            std::max<std::uint32_t>(1, (previous.height + 1) / 2);
        generated.basePixelsPerTexelX =
            static_cast<double>(baseWidth) / generated.width;
        generated.basePixelsPerTexelY =
            static_cast<double>(baseHeight) / generated.height;
        generated.kind = RasterLevelKind::GeneratedCoverage;
        levels.push_back(generated);
        previous = generated;
    }
}

std::size_t
rasterBackedLevelCount(const std::span<const RasterLevel> levels) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(
        levels, RasterLevelKind::Backed, &RasterLevel::kind));
}

std::size_t
rasterGeneratedLevelCount(const std::span<const RasterLevel> levels) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(
        levels, RasterLevelKind::GeneratedCoverage, &RasterLevel::kind));
}

RasterLayerStyle defaultRasterLayerStyle(const RasterLayerMetadata &metadata)
{
    RasterLayerStyle style;
    style.displayRange = metadata.defaultDisplay.displayRange;
    if (metadata.defaultDisplay.sampleKind ==
        RasterSampleKind::ContinuousScalar) {
        style.colorRampKey = defaultRasterScalarColorRampKey;
    }
    return clampRasterLayerStyle(std::move(style));
}

Bounds3d
rasterSceneBounds(const RasterLayerMetadata &metadata,
                  const RasterLayerStyle &style,
                  const RasterElevationStatus elevationStatus,
                  const std::optional<RasterElevationRange> exactRange) noexcept
{
    Bounds3d result = metadata.bounds;
    if (rasterEffectiveRenderMode(
            metadata, style, elevationStatus, exactRange) ==
        RasterRenderMode::Surface) {
        const double minimum =
            exactRange->minimum * style.verticalExaggeration + style.zOffset;
        const double maximum =
            exactRange->maximum * style.verticalExaggeration + style.zOffset;
        result.minimum[2] = std::min(minimum, maximum);
        result.maximum[2] = std::max(minimum, maximum);
        if (result.minimum[2] == result.maximum[2]) {
            result.minimum[2] -= 0.5;
            result.maximum[2] += 0.5;
        }
        return result;
    }
    result.minimum[2] = style.zOffset - 0.5;
    result.maximum[2] = style.zOffset + 0.5;
    return result;
}

RasterRenderMode rasterEffectiveRenderMode(
    const RasterLayerMetadata &metadata,
    const RasterLayerStyle &style,
    const RasterElevationStatus elevationStatus,
    const std::optional<RasterElevationRange> &exactRange) noexcept
{
    return style.renderMode == RasterRenderMode::Surface &&
                   metadata.elevation.available &&
                   elevationStatus == RasterElevationStatus::Ready && exactRange
               ? RasterRenderMode::Surface
               : RasterRenderMode::Flat;
}

bool rasterDecodeAffectedBy(const RasterLayerStyle &before,
                            const RasterLayerStyle &after) noexcept
{
    return before.displayRange != after.displayRange ||
           before.colorRampKey != after.colorRampKey;
}

RasterLayerStyle clampRasterLayerStyle(RasterLayerStyle style)
{
    if (!std::isfinite(style.opacity)) {
        style.opacity = 1.0F;
    }
    style.opacity = std::clamp(style.opacity, 0.0F, 1.0F);

    if (!std::isfinite(style.zOffset)) {
        style.zOffset = 0.0;
    }
    style.zOffset = std::clamp(style.zOffset,
                               -maximumRasterZOffsetMagnitude,
                               maximumRasterZOffsetMagnitude);

    if (!std::isfinite(style.verticalExaggeration)) {
        style.verticalExaggeration = 1.0;
    }
    style.verticalExaggeration = std::clamp(style.verticalExaggeration,
                                            minimumRasterVerticalExaggeration,
                                            maximumRasterVerticalExaggeration);

    if (!std::isfinite(style.surfaceShadingStrength)) {
        style.surfaceShadingStrength = 1.0F;
    }
    style.surfaceShadingStrength =
        std::clamp(style.surfaceShadingStrength, 0.0F, 1.0F);

    if (style.displayRange) {
        RasterDisplayRange &range = *style.displayRange;
        if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum)) {
            style.displayRange.reset();
        } else if (range.minimum > range.maximum) {
            std::swap(range.minimum, range.maximum);
        }
    }
    return style;
}

} // namespace pci

std::size_t std::hash<pci::RasterTileKey>::operator()(
    const pci::RasterTileKey &key) const noexcept
{
    std::size_t seed = pci::hashCombine(0, key.levelIndex);
    seed = pci::hashCombine(seed, key.x);
    return pci::hashCombine(seed, key.y);
}
