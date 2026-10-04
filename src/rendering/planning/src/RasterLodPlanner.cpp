#include <pci/rendering/planning/RasterLodPlanner.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_set>

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

[[nodiscard]] double
cross(const Point2 origin, const Point2 a, const Point2 b) noexcept
{
    return (a[0] - origin[0]) * (b[1] - origin[1]) -
           (a[1] - origin[1]) * (b[0] - origin[0]);
}

[[nodiscard]] std::vector<Point2> convexHull(std::vector<Point2> points)
{
    std::ranges::sort(points, [](const Point2 left, const Point2 right) {
        return left[0] < right[0] ||
               (left[0] == right[0] && left[1] < right[1]);
    });
    points.erase(std::ranges::unique(points).begin(), points.end());
    if (points.size() < 3) {
        return {};
    }
    std::vector<Point2> hull;
    hull.reserve(points.size() * 2);
    for (const Point2 point : points) {
        while (hull.size() >= 2 &&
               cross(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    const std::size_t lower = hull.size();
    for (auto point = points.rbegin(); point != points.rend(); ++point) {
        while (hull.size() > lower &&
               cross(hull[hull.size() - 2], hull.back(), *point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(*point);
    }
    hull.pop_back();
    return hull.size() < 3 ? std::vector<Point2>{} : hull;
}

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
             const PixelRect &bounds,
             const std::size_t maximumCells,
             bool *const truncated = nullptr)
{
    if (truncated) {
        *truncated = false;
    }
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
            const PixelRect cell = tileBaseRect(metadata, key);
            if (cell.overlaps(bounds) &&
                convexPolygonOverlapsRect(polygon, cell)) {
                if (cells.size() >= maximumCells) {
                    if (truncated) {
                        *truncated = true;
                    }
                    return cells;
                }
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

std::vector<Point2> rasterVisiblePixelPolygon(const RasterLodLayerView &layer,
                                              const FrameCamera &camera)
{
    const RasterLayerMetadata &metadata = layer.metadata;
    if (metadata.width == 0 || metadata.height == 0 ||
        !rasterAffineInvertible(metadata.geoTransform)) {
        return {};
    }

    const RasterRenderMode mode =
        rasterEffectiveRenderMode(metadata,
                                  layer.style,
                                  layer.elevationStatus,
                                  layer.exactElevationRange);
    if (mode == RasterRenderMode::Surface) {
        const double low = layer.exactElevationRange->minimum *
                               layer.style.verticalExaggeration +
                           layer.style.zOffset;
        const double high = layer.exactElevationRange->maximum *
                                layer.style.verticalExaggeration +
                            layer.style.zOffset;
        std::array<Vec3d, 4> floor = rasterCornerPoints(
            metadata.geoTransform, metadata.width, metadata.height);
        std::array<Vec3d, 4> ceiling = floor;
        for (Vec3d &corner : floor) {
            corner.z = std::min(low, high);
        }
        for (Vec3d &corner : ceiling) {
            corner.z = std::max(low, high);
        }
        const std::array<std::array<Vec3d, 4>, 6> faces{
            floor,
            ceiling,
            std::array<Vec3d, 4>{floor[0], floor[1], ceiling[1], ceiling[0]},
            std::array<Vec3d, 4>{floor[1], floor[2], ceiling[2], ceiling[1]},
            std::array<Vec3d, 4>{floor[2], floor[3], ceiling[3], ceiling[2]},
            std::array<Vec3d, 4>{floor[3], floor[0], ceiling[0], ceiling[3]},
        };
        std::vector<Vec3d> intersectionVertices;
        for (const auto &face : faces) {
            std::vector<Vec3d> clipped = camera.culler.clipConvexPolygon(face);
            intersectionVertices.insert(
                intersectionVertices.end(), clipped.begin(), clipped.end());
        }
        // Clipped prism faces alone are not conservative when a frustum corner
        // lies inside the prism. Include those original intersection vertices.
        for (const Vec3d corner : camera.culler.corners()) {
            if (corner.z < floor[0].z || corner.z > ceiling[0].z) {
                continue;
            }
            const auto pixel =
                rasterWorldToPixel(metadata.geoTransform, corner.x, corner.y);
            if (pixel && pixel->pixel >= 0.0 &&
                pixel->pixel <= static_cast<double>(metadata.width) &&
                pixel->line >= 0.0 &&
                pixel->line <= static_cast<double>(metadata.height)) {
                intersectionVertices.push_back(corner);
            }
        }

        std::vector<Point2> pixels;
        pixels.reserve(intersectionVertices.size());
        for (const Vec3d vertex : intersectionVertices) {
            const auto pixel =
                rasterWorldToPixel(metadata.geoTransform, vertex.x, vertex.y);
            if (pixel) {
                pixels.push_back({
                    std::clamp(
                        pixel->pixel, 0.0, static_cast<double>(metadata.width)),
                    std::clamp(
                        pixel->line, 0.0, static_cast<double>(metadata.height)),
                });
            }
        }
        if (std::vector<Point2> hull = convexHull(std::move(pixels));
            hull.size() >= 3) {
            return hull;
        }
        if (!camera.culler.intersects(
                rasterSceneBounds(metadata,
                                  layer.style,
                                  layer.elevationStatus,
                                  layer.exactElevationRange))) {
            return {};
        }
        // Degenerate/contained intersections deliberately over-plan rather than
        // excluding terrain that can become visible through its height.
        return {{0.0, 0.0},
                {static_cast<double>(metadata.width), 0.0},
                {static_cast<double>(metadata.width),
                 static_cast<double>(metadata.height)},
                {0.0, static_cast<double>(metadata.height)}};
    }

    // Flat retains the existing finite-quad clipping path.
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
    const RasterLayerMetadata &metadata = input.layer.metadata;
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

    // Generated coverage pyramids end in a single-tile logical root. Sources
    // created before that facility may still begin with several coarse cells,
    // so enumeration remains bounded by the effective cap.
    std::deque<RasterTileKey> pending;
    std::unordered_set<RasterTileKey> seenCells;
    bool coarseTruncated = false;
    for (const RasterTileKey key : visibleCells(metadata,
                                                coarsest,
                                                polygon,
                                                visibleBounds,
                                                effectiveCap,
                                                &coarseTruncated)) {
        pending.push_back(key);
        seenCells.insert(key);
    }
    if (coarseTruncated) {
        plan.capacityLimited = true;
        plan.coverageIncomplete = true;
    }

    const auto unavailable = [&input](const RasterTileKey key) {
        return input.unavailable && input.unavailable(key);
    };

    const auto desiredLevel = [&](const RasterTileKey key) {
        const PixelRect cell = tileBaseRect(metadata, key);
        const Vec3d center = tileCenterWorld(metadata, input.layer.style, cell);
        const auto texelPixels = [&](const std::uint32_t level) {
            if (rasterEffectiveRenderMode(metadata,
                                          input.layer.style,
                                          input.layer.elevationStatus,
                                          input.layer.exactElevationRange) ==
                RasterRenderMode::Surface) {
                const double low = input.layer.exactElevationRange->minimum *
                                       input.layer.style.verticalExaggeration +
                                   input.layer.style.zOffset;
                const double high = input.layer.exactElevationRange->maximum *
                                        input.layer.style.verticalExaggeration +
                                    input.layer.style.zOffset;
                return std::max(
                    rasterProjectedTexelPixels(metadata,
                                               metadata.levels[level],
                                               {center.x, center.y, low},
                                               input.camera),
                    rasterProjectedTexelPixels(metadata,
                                               metadata.levels[level],
                                               {center.x, center.y, high},
                                               input.camera));
            }
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
        const bool keyUnavailable = unavailable(key);
        if (key.levelIndex == 0) {
            if (keyUnavailable) {
                plan.coverageIncomplete = true;
            } else {
                plan.selected.push_back(key);
            }
            continue;
        }
        if (desired >= key.levelIndex && !keyUnavailable) {
            plan.selected.push_back(key);
            continue;
        }

        const std::size_t occupied = plan.selected.size() + pending.size();
        const std::size_t remaining =
            occupied < effectiveCap ? effectiveCap - occupied : 0;
        bool enumerationTruncated = false;
        const std::vector<RasterTileKey> candidates =
            visibleCells(metadata,
                         key.levelIndex - 1,
                         polygon,
                         tileBaseRect(metadata, key),
                         effectiveCap,
                         &enumerationTruncated);
        std::vector<RasterTileKey> children;
        children.reserve(candidates.size());
        for (const RasterTileKey child : candidates) {
            if (!seenCells.contains(child)) {
                children.push_back(child);
            }
        }
        const bool childrenTruncated =
            enumerationTruncated || children.size() > remaining;
        // Arbitrarily large gaps between overviews can turn one parent into
        // millions of child candidates, so the count is checked before the
        // children are materialized into the plan.
        if (childrenTruncated) {
            plan.capacityLimited = true;
            if (keyUnavailable) {
                // A failed generated tile cannot be retained as the coverage
                // fallback. Use every child that fits and report the genuine
                // coverage gap; subsequent frames can continue below them.
                plan.coverageIncomplete = true;
                children.resize(std::min(children.size(), remaining));
                for (const RasterTileKey child : children) {
                    seenCells.insert(child);
                    pending.push_back(child);
                }
            } else {
                plan.selected.push_back(key);
            }
            continue;
        }
        for (const RasterTileKey child : children) {
            seenCells.insert(child);
            pending.push_back(child);
        }
    }

    std::ranges::sort(plan.selected);
    plan.selected.erase(std::ranges::unique(plan.selected).begin(),
                        plan.selected.end());
    if (plan.selected.size() > effectiveCap) {
        plan.capacityLimited = true;
        plan.coverageIncomplete = true;
        plan.selected.resize(effectiveCap);
    }

    const auto resident = [&input](const RasterTileKey key) {
        return input.gpuResident && input.gpuResident(key);
    };
    const auto decoded = [&input](const RasterTileKey key) {
        return input.cpuResident && input.cpuResident(key);
    };

    // A resident coarser ancestor stays visible until every selected child is
    // ready, which is what prevents holes and flashes during refinement.
    for (const RasterTileKey key : plan.selected) {
        if (unavailable(key)) {
            plan.coverageIncomplete = true;
            continue;
        }
        if (resident(key)) {
            plan.draw.push_back(key);
            plan.protectedTiles.push_back(key);
            continue;
        }
        const bool targetDecoded = decoded(key);
        if (targetDecoded) {
            plan.decodedUploads.push_back(key);
            plan.protectedTiles.push_back(key);
        } else {
            plan.requests.push_back(key);
        }
        bool decodedFallbackAvailable = targetDecoded;

        const PixelRect cell = tileBaseRect(metadata, key);
        for (std::uint32_t level = key.levelIndex + 1;
             level < metadata.levels.size();
             ++level) {
            // The ancestor relation is geometric, not x / 2: adjacent GDAL
            // levels may reduce by any ratio.
            bool found = false;
            for (const RasterTileKey candidate :
                 visibleCells(metadata, level, polygon, cell, effectiveCap)) {
                if (unavailable(candidate)) {
                    continue;
                }
                if (resident(candidate)) {
                    plan.draw.push_back(candidate);
                    plan.protectedTiles.push_back(candidate);
                    found = true;
                    break;
                }
                if (decoded(candidate)) {
                    plan.decodedUploads.push_back(candidate);
                    plan.protectedTiles.push_back(candidate);
                    decodedFallbackAvailable = true;
                    // Keep walking: an even coarser GPU-resident ancestor can
                    // remain visible while this decoded fallback is uploaded.
                    continue;
                }
                // Missing ancestors are requested as well as the detail tile.
                // The final priority sort puts these coarser keys first, so an
                // initially blank close view gains coverage before sharpness.
                if (!decodedFallbackAvailable) {
                    plan.requests.push_back(candidate);
                }
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
    std::ranges::sort(plan.decodedUploads);
    plan.decodedUploads.erase(std::ranges::unique(plan.decodedUploads).begin(),
                              plan.decodedUploads.end());
    std::ranges::sort(plan.requests);
    plan.requests.erase(std::ranges::unique(plan.requests).begin(),
                        plan.requests.end());

    // Coarse coverage first, then detail ordered by distance to the viewport
    // center, so the view fills in before it sharpens.
    const Vec3d focus = tileCenterWorld(metadata,
                                        input.layer.style,
                                        PixelRect{visibleBounds.minimumX,
                                                  visibleBounds.minimumY,
                                                  visibleBounds.maximumX,
                                                  visibleBounds.maximumY});
    const auto priority = [&](const RasterTileKey left,
                              const RasterTileKey right) {
        if (left.levelIndex != right.levelIndex) {
            return left.levelIndex > right.levelIndex;
        }
        const Vec3d leftCenter = tileCenterWorld(
            metadata, input.layer.style, tileBaseRect(metadata, left));
        const Vec3d rightCenter = tileCenterWorld(
            metadata, input.layer.style, tileBaseRect(metadata, right));
        return dot(leftCenter - focus, leftCenter - focus) <
               dot(rightCenter - focus, rightCenter - focus);
    };
    std::ranges::sort(plan.requests, priority);
    std::ranges::sort(plan.decodedUploads, priority);

    return plan;
}

} // namespace pci
