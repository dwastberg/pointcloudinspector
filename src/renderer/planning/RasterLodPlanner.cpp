#include "renderer/planning/RasterLodPlanner.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>

namespace pci {
namespace {

using Point2 = std::array<double, 2>;

struct PixelRect {
    double minimumX = 0.0;
    double minimumY = 0.0;
    double maximumX = 0.0;
    double maximumY = 0.0;

    [[nodiscard]] bool overlaps(const PixelRect &other) const noexcept
    {
        return minimumX < other.maximumX && other.minimumX < maximumX &&
               minimumY < other.maximumY && other.minimumY < maximumY;
    }
};

[[nodiscard]] PixelRect polygonBounds(const std::vector<Point2> &polygon)
{
    PixelRect bounds{std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::lowest(),
                     std::numeric_limits<double>::lowest()};
    for (const Point2 &vertex : polygon) {
        bounds.minimumX = std::min(bounds.minimumX, vertex[0]);
        bounds.minimumY = std::min(bounds.minimumY, vertex[1]);
        bounds.maximumX = std::max(bounds.maximumX, vertex[0]);
        bounds.maximumY = std::max(bounds.maximumY, vertex[1]);
    }
    return bounds;
}

// Separating-axis test between a convex polygon and an axis-aligned rect. The
// rect's own axes are tested first, then each polygon edge normal, so a thin
// rotated footprint does not claim every tile inside its much larger
// axis-aligned bounds.
[[nodiscard]] bool convexPolygonOverlapsRect(const std::vector<Point2> &polygon,
                                             const PixelRect &rect)
{
    if (polygon.size() < 3) {
        return false;
    }
    if (!polygonBounds(polygon).overlaps(rect)) {
        return false;
    }
    const std::array<Point2, 4> corners{Point2{rect.minimumX, rect.minimumY},
                                        Point2{rect.maximumX, rect.minimumY},
                                        Point2{rect.maximumX, rect.maximumY},
                                        Point2{rect.minimumX, rect.maximumY}};

    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const Point2 start = polygon[index];
        const Point2 end = polygon[(index + 1) % polygon.size()];
        const Point2 axis{-(end[1] - start[1]), end[0] - start[0]};
        if (axis[0] == 0.0 && axis[1] == 0.0) {
            continue;
        }
        double polygonLow = std::numeric_limits<double>::max();
        double polygonHigh = std::numeric_limits<double>::lowest();
        for (const Point2 &vertex : polygon) {
            const double value = vertex[0] * axis[0] + vertex[1] * axis[1];
            polygonLow = std::min(polygonLow, value);
            polygonHigh = std::max(polygonHigh, value);
        }
        double rectLow = std::numeric_limits<double>::max();
        double rectHigh = std::numeric_limits<double>::lowest();
        for (const Point2 &corner : corners) {
            const double value = corner[0] * axis[0] + corner[1] * axis[1];
            rectLow = std::min(rectLow, value);
            rectHigh = std::max(rectHigh, value);
        }
        if (polygonHigh < rectLow || rectHigh < polygonLow) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] PixelRect tileBaseRect(const RasterLayerMetadata &metadata,
                                     const RasterTileKey key)
{
    const RasterBasePixelRect rect = rasterTileBasePixelRect(
        metadata.levels[key.levelIndex], key, metadata.width, metadata.height);
    return PixelRect{rect.minimumPixel,
                     rect.minimumLine,
                     rect.maximumPixel,
                     rect.maximumLine};
}

[[nodiscard]] Vec3d tileCenterWorld(const RasterLayerMetadata &metadata,
                                    const RasterLayerStyle &style,
                                    const PixelRect &rect)
{
    const Vec3d center =
        rasterPixelToWorld(metadata.geoTransform,
                           (rect.minimumX + rect.maximumX) * 0.5,
                           (rect.minimumY + rect.maximumY) * 0.5);
    return Vec3d{center.x, center.y, style.zOffset};
}

// Cells at one level whose base-pixel extent overlaps the visible polygon,
// enumerated from checked row spans rather than a bitmap proportional to the
// raster dimensions. The range is expanded by one tile for prefetch.
[[nodiscard]] std::vector<RasterTileKey>
visibleCells(const RasterLayerMetadata &metadata,
             const std::uint32_t levelIndex,
             const std::vector<Point2> &polygon,
             const PixelRect &bounds)
{
    const RasterLevel &level = metadata.levels[levelIndex];
    const std::uint32_t countX = rasterLevelTileCountX(level);
    const std::uint32_t countY = rasterLevelTileCountY(level);
    if (countX == 0 || countY == 0) {
        return {};
    }

    const double tileBasePixelsX = rasterTilePixels * level.basePixelsPerTexelX;
    const double tileBasePixelsY = rasterTilePixels * level.basePixelsPerTexelY;
    if (!(tileBasePixelsX > 0.0) || !(tileBasePixelsY > 0.0)) {
        return {};
    }

    const auto span = [](const double low,
                         const double high,
                         const double step,
                         const std::uint32_t count) {
        // Never cast a negative or non-finite coordinate to an unsigned index.
        const double first = std::floor(low / step) - 1.0;
        const double last = std::floor(high / step) + 1.0;
        const auto begin = static_cast<std::uint32_t>(
            std::clamp(first, 0.0, static_cast<double>(count - 1)));
        const auto end = static_cast<std::uint32_t>(
            std::clamp(last, 0.0, static_cast<double>(count - 1)));
        return std::pair{begin, end};
    };

    const auto [beginX, endX] =
        span(bounds.minimumX, bounds.maximumX, tileBasePixelsX, countX);
    const auto [beginY, endY] =
        span(bounds.minimumY, bounds.maximumY, tileBasePixelsY, countY);

    std::vector<RasterTileKey> cells;
    for (std::uint32_t y = beginY; y <= endY; ++y) {
        for (std::uint32_t x = beginX; x <= endX; ++x) {
            const RasterTileKey key{levelIndex, x, y};
            if (convexPolygonOverlapsRect(polygon,
                                          tileBaseRect(metadata, key))) {
                cells.push_back(key);
            }
        }
    }
    return cells;
}

// The finest previously selected level overlapping this cell's base-pixel
// extent, or nothing when the region is new. Looking the prior level up by key
// equality would miss whenever the candidate level differs from last frame's,
// which is most frames during a zoom.
[[nodiscard]] std::optional<std::uint32_t>
priorLevelFor(const RasterLayerMetadata &metadata,
              const std::span<const RasterTileKey> previousSelection,
              const PixelRect &cell)
{
    std::optional<std::uint32_t> finest;
    for (const RasterTileKey key : previousSelection) {
        if (key.levelIndex >= metadata.levels.size()) {
            continue;
        }
        if (!tileBaseRect(metadata, key).overlaps(cell)) {
            continue;
        }
        finest = finest ? std::min(*finest, key.levelIndex) : key.levelIndex;
    }
    return finest;
}

} // namespace

double rasterProjectedTexelPixels(const RasterLayerMetadata &metadata,
                                  const RasterLevel &level,
                                  const Vec3d worldPosition,
                                  const FrameCamera &camera)
{
    // One texel step at this level, in world units, through the same six-term
    // affine as the raster itself.
    const Vec3d stepX{metadata.geoTransform[1] * level.basePixelsPerTexelX,
                      metadata.geoTransform[4] * level.basePixelsPerTexelX,
                      0.0};
    const Vec3d stepY{metadata.geoTransform[2] * level.basePixelsPerTexelY,
                      metadata.geoTransform[5] * level.basePixelsPerTexelY,
                      0.0};
    const double texelWorld = std::max(length(stepX), length(stepY));

    const double depth = dot(worldPosition - camera.eye, camera.forward);
    const double worldPerPixel = camera.worldUnitsPerPixelAtDepth(depth);
    if (!std::isfinite(worldPerPixel) || worldPerPixel <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return texelWorld / worldPerPixel;
}

std::vector<Point2> rasterVisiblePixelPolygon(const RasterLayer &layer,
                                              const FrameCamera &camera)
{
    if (!layer.data) {
        return {};
    }
    const RasterLayerMetadata &metadata = layer.data->metadata();
    if (metadata.width == 0 || metadata.height == 0 ||
        !rasterAffineInvertible(metadata.geoTransform)) {
        return {};
    }

    // The raster's four finite corners at the styled elevation, clipped
    // against the frustum rather than traversed from a dataset-wide root.
    std::array<Vec3d, 4> corners = rasterCornerPoints(
        metadata.geoTransform, metadata.width, metadata.height);
    for (Vec3d &corner : corners) {
        corner.z = layer.style.zOffset;
    }
    const std::vector<Vec3d> clipped = camera.culler.clipConvexPolygon(corners);
    if (clipped.size() < 3) {
        return {};
    }

    std::vector<Point2> pixels;
    pixels.reserve(clipped.size());
    for (const Vec3d vertex : clipped) {
        const auto pixel =
            rasterWorldToPixel(metadata.geoTransform, vertex.x, vertex.y);
        if (!pixel) {
            return {};
        }
        pixels.push_back(Point2{
            std::clamp(pixel->pixel, 0.0, static_cast<double>(metadata.width)),
            std::clamp(
                pixel->line, 0.0, static_cast<double>(metadata.height))});
    }
    return pixels;
}

RasterLodPlan planRasterTiles(const RasterLodPlanInput &input)
{
    RasterLodPlan plan;
    if (!input.layer.data) {
        return plan;
    }
    const RasterLayerMetadata &metadata = input.layer.data->metadata();
    if (metadata.levels.empty()) {
        return plan;
    }

    const std::vector<Point2> polygon =
        rasterVisiblePixelPolygon(input.layer, input.camera);
    if (polygon.size() < 3) {
        return plan;
    }
    const PixelRect visibleBounds = polygonBounds(polygon);

    const std::size_t effectiveCap = std::max<std::size_t>(
        1, std::min(input.maximumSelectedTiles, input.gpuCapacityTiles));
    const auto coarsest =
        static_cast<std::uint32_t>(metadata.levels.size() - 1);

    // Refinement starts from the coarsest backed level; nothing walks from a
    // dataset-wide root, so the work is proportional to visible coverage.
    std::deque<RasterTileKey> pending;
    for (const RasterTileKey key :
         visibleCells(metadata, coarsest, polygon, visibleBounds)) {
        pending.push_back(key);
    }
    if (pending.size() > effectiveCap) {
        plan.capacityLimited = true;
        plan.insufficientOverviews = true;
        pending.resize(effectiveCap);
    }

    const auto desiredLevel = [&](const RasterTileKey key) {
        const PixelRect cell = tileBaseRect(metadata, key);
        const Vec3d center = tileCenterWorld(metadata, input.layer.style, cell);
        const auto texelPixels = [&](const std::uint32_t level) {
            return rasterProjectedTexelPixels(
                metadata, metadata.levels[level], center, input.camera);
        };

        std::uint32_t level = key.levelIndex;
        if (const auto prior =
                priorLevelFor(metadata, input.previousSelection, cell)) {
            level = std::min(*prior, coarsest);
        } else {
            // No prior selection: take the coarsest level at or under the
            // refine threshold.
            level = 0;
            for (std::uint32_t candidate = coarsest;; --candidate) {
                if (texelPixels(candidate) <= rasterRefineTexelPixels) {
                    level = candidate;
                    break;
                }
                if (candidate == 0) {
                    break;
                }
            }
            return level;
        }

        // Hysteresis: refine against the current level, coarsen against the
        // candidate. Comparing both thresholds with the current level can flap
        // or create a dead zone when overview ratios are not 2x.
        while (level > 0 && texelPixels(level) > rasterRefineTexelPixels) {
            --level;
        }
        while (level < coarsest &&
               texelPixels(level + 1) < rasterCoarsenTexelPixels) {
            ++level;
        }
        return level;
    };

    while (!pending.empty()) {
        const RasterTileKey key = pending.front();
        pending.pop_front();

        const std::uint32_t desired = desiredLevel(key);
        if (desired >= key.levelIndex || key.levelIndex == 0) {
            plan.selected.push_back(key);
            continue;
        }

        const std::vector<RasterTileKey> children = visibleCells(
            metadata, key.levelIndex - 1, polygon, tileBaseRect(metadata, key));
        // Arbitrarily large gaps between overviews can turn one parent into
        // millions of child candidates, so the count is checked before the
        // children are materialized into the plan.
        if (plan.selected.size() + pending.size() + children.size() >
            effectiveCap) {
            plan.capacityLimited = true;
            plan.selected.push_back(key);
            continue;
        }
        for (const RasterTileKey child : children) {
            pending.push_back(child);
        }
    }

    std::ranges::sort(plan.selected);
    plan.selected.erase(std::ranges::unique(plan.selected).begin(),
                        plan.selected.end());

    const auto resident = [&input](const RasterTileKey key) {
        return input.gpuResident && input.gpuResident(key);
    };

    // A resident coarser ancestor stays visible until every selected child is
    // ready, which is what prevents holes and flashes during refinement.
    for (const RasterTileKey key : plan.selected) {
        if (resident(key)) {
            plan.draw.push_back(key);
            plan.protectedTiles.push_back(key);
            continue;
        }
        plan.requests.push_back(key);

        const PixelRect cell = tileBaseRect(metadata, key);
        for (std::uint32_t level = key.levelIndex + 1;
             level < metadata.levels.size();
             ++level) {
            // The ancestor relation is geometric, not x / 2: adjacent GDAL
            // levels may reduce by any ratio.
            bool found = false;
            for (const RasterTileKey candidate :
                 visibleCells(metadata, level, polygon, cell)) {
                if (!resident(candidate)) {
                    continue;
                }
                plan.draw.push_back(candidate);
                plan.protectedTiles.push_back(candidate);
                found = true;
                break;
            }
            if (found) {
                break;
            }
        }
    }

    std::ranges::sort(plan.draw);
    plan.draw.erase(std::ranges::unique(plan.draw).begin(), plan.draw.end());
    std::ranges::sort(plan.protectedTiles);
    plan.protectedTiles.erase(std::ranges::unique(plan.protectedTiles).begin(),
                              plan.protectedTiles.end());

    // Coarse coverage first, then detail ordered by distance to the viewport
    // center, so the view fills in before it sharpens.
    const Vec3d focus = tileCenterWorld(metadata,
                                        input.layer.style,
                                        PixelRect{visibleBounds.minimumX,
                                                  visibleBounds.minimumY,
                                                  visibleBounds.maximumX,
                                                  visibleBounds.maximumY});
    std::ranges::sort(
        plan.requests,
        [&](const RasterTileKey left, const RasterTileKey right) {
            if (left.levelIndex != right.levelIndex) {
                return left.levelIndex > right.levelIndex;
            }
            const Vec3d leftCenter = tileCenterWorld(
                metadata, input.layer.style, tileBaseRect(metadata, left));
            const Vec3d rightCenter = tileCenterWorld(
                metadata, input.layer.style, tileBaseRect(metadata, right));
            return dot(leftCenter - focus, leftCenter - focus) <
                   dot(rightCenter - focus, rightCenter - focus);
        });

    if (plan.selected.size() > effectiveCap) {
        plan.capacityLimited = true;
        plan.selected.resize(effectiveCap);
    }
    return plan;
}

} // namespace pci
