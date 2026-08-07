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
        levels.front().height != baseHeight) {
        return false;
    }
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
    const RasterLevel &coarsest = metadata.levels.back();
    if (coarsest.width <= rasterStaticTextureLimitPixels &&
        coarsest.height <= rasterStaticTextureLimitPixels) {
        return false;
    }
    // No backed level fits, so the only remaining option is a decimating read
    // of the base band. That is bounded exactly when the base is bounded.
    const auto basePixels =
        static_cast<std::uint64_t>(metadata.width) * metadata.height;
    return basePixels > rasterBoundedBaseReadPixels;
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
