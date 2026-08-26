#pragma once

#include "foundation/Bounds3d.h"
#include "foundation/StrongId.h"
#include "foundation/Vec3d.h"
#include "pointcloud/PointColorMapCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pci {

inline constexpr std::uint32_t rasterTilePixels = 256;
inline constexpr std::uint32_t rasterTileGutter = 1;
inline constexpr std::uint32_t rasterStoredTilePixels =
    rasterTilePixels + 2 * rasterTileGutter;
static_assert(rasterStoredTilePixels == 258);

// Above this base-image size, a decimating read of the base band is no longer
// bounded and only an explicitly backed overview may be used for inspection
// sampling. Runtime backed-level tiles remain 1:1; generated coverage tiles
// perform bounded, on-demand decimation into one fixed-size tile buffer.
inline constexpr std::uint64_t rasterBoundedBaseReadPixels = 64ULL << 20;

// A level at or under this size covers the whole raster in at most 16x16
// tiles, which is what makes a zoomed-out view affordable. Above it, a source
// with no coarser level is reported as under-overviewed.
inline constexpr std::uint32_t rasterOverviewCoverageLimitPixels = 4096;

inline constexpr double maximumRasterZOffsetMagnitude = 1.0e6;
inline constexpr double minimumRasterVerticalExaggeration = 0.1;
inline constexpr double maximumRasterVerticalExaggeration = 100.0;

// A single continuous scalar band is colorized rather than presented as if it
// were photographic grayscale.
inline constexpr std::string_view defaultRasterScalarColorRampKey =
    "cpt:viridis.cpt";

using RasterSourceId = StrongId<struct RasterSourceIdTag>;

class RasterImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RasterImportCancelled final : public RasterImportError {
public:
    using RasterImportError::RasterImportError;
};

enum class RasterSampleKind : std::uint8_t {
    ContinuousColor,
    ContinuousScalar,
    Categorical,
};

enum class RasterRenderMode : std::uint8_t {
    Flat,
    Surface,
};

enum class RasterElevationStatus : std::uint8_t {
    NotApplicable,
    Unknown,
    Scanning,
    Ready,
    Failed,
};

struct RasterElevationRange {
    double minimum = 0.0;
    double maximum = 0.0;
    bool operator==(const RasterElevationRange &) const = default;
};

struct RasterElevationDescriptor {
    bool available = false;
    int band = 0;
    double scale = 1.0;
    double offset = 0.0;
    std::string unit;
    // Subtracted before float storage. It is a precision anchor, never a bound.
    double anchor = 0.0;
    // Trustworthy, non-approximate cached statistics make Surface immediately
    // ready without a redundant full-raster scan.
    std::optional<RasterElevationRange> cachedExactRange;
    bool operator==(const RasterElevationDescriptor &) const = default;
};

struct RasterBandRef {
    int band = 0;      // GDAL's one-based band number
    int overview = -1; // -1 means base band
    bool operator==(const RasterBandRef &) const = default;
};

enum class RasterLevelKind : std::uint8_t {
    Backed,
    GeneratedCoverage,
};

// One entry of the inspected level table. Entry 0 is the full-resolution band;
// later entries are ordered from finer to coarser resolution. A level index
// never implies a power-of-two reduction: the measured base-pixels-per-texel
// ratios are the only reduction the rest of the system may use.
struct RasterLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double basePixelsPerTexelX = 1.0;
    double basePixelsPerTexelY = 1.0;
    std::uint8_t channelCount = 0;
    std::array<RasterBandRef, 4> rgbaBands{};
    std::optional<RasterBandRef> maskBand;
    // Backed levels name dimensions physically exposed by GDAL. Generated
    // coverage levels are logical, read-only levels resampled on demand from
    // the coarsest backed level; they are never written to the source.
    RasterLevelKind kind = RasterLevelKind::Backed;
    bool operator==(const RasterLevel &) const = default;
};

struct RasterDisplayRange {
    double minimum = 0.0;
    double maximum = 1.0;
    enum class Origin : std::uint8_t {
        Metadata,
        CachedStatistics,
        Sampled
    };
    Origin origin = Origin::Metadata;
    bool operator==(const RasterDisplayRange &) const = default;
};

// Immutable and shared by every request in one render generation, so a ramp is
// not copied per tile and stays alive until active workers finish. A palette
// color table is deliberately absent: it is a fixed property of the source
// rather than of the user-adjustable display transform, and expansion happens
// inside the GDAL adapter before portable code sees a tile.
struct RasterDecodeParameters {
    RasterSampleKind sampleKind = RasterSampleKind::ContinuousColor;
    std::optional<RasterDisplayRange> displayRange;
    std::shared_ptr<const std::vector<PointColorStop>> colorRamp;
};

struct RasterBandInfo {
    int band = 0;
    std::string name;
    std::string dataType;
    std::string colorInterpretation;
    std::optional<double> nodata;
    bool operator==(const RasterBandInfo &) const = default;
};

struct RasterLayerMetadata {
    std::filesystem::path sourcePath;
    std::string sourceDriver;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<double, 6> geoTransform{};
    // Length of the affine column and row basis vectors. Unlike abs(gt[1])
    // and abs(gt[5]), these remain correct for rotation and skew.
    std::array<double, 2> nativePixelSize{};
    Bounds3d bounds;
    std::string spatialReferenceWkt;
    std::vector<RasterBandInfo> bands;
    std::vector<RasterLevel> levels;
    RasterDecodeParameters defaultDisplay;
    RasterElevationDescriptor elevation;
    bool geographicCrs = false;
    bool crsMissing = false;
    bool crsMismatch = false;
    bool extentDisjointXY = false;
    bool insufficientOverviews = false;
    bool crossesAntimeridian = false;
    // Three/four otherwise-unlabelled equal-type bands were interpreted as
    // RGB(A) by position. The layer remains usable, but the assignment is a
    // user-visible warning rather than a silent claim about source semantics.
    bool positionalBandFallback = false;
};

struct RasterLayerStyle {
    float opacity = 1.0F;
    double zOffset = 0.0;
    std::optional<RasterDisplayRange> displayRange;
    std::string colorRampKey; // empty for RGB; stable CPT key for scalar data
    RasterRenderMode renderMode = RasterRenderMode::Flat;
    double verticalExaggeration = 1.0;
    float surfaceShadingStrength = 1.0F;
    bool operator==(const RasterLayerStyle &) const = default;
};

// True when the source has no level coarse enough to show the whole raster
// without reading an unreasonable number of native-resolution tiles, and its
// base is too large to be worth covering that way. Such a source is admitted,
// receives an automatic in-memory coverage pyramid, and warns that real source
// overviews would make zoomed-out navigation faster and sharper.
//
// Everything renders through the tiled path now, so this is a display-quality
// warning rather than a decision between two read strategies.
[[nodiscard]] bool
rasterRequiresTiledRendering(const RasterLayerMetadata &metadata) noexcept;

// Appends a halving pyramid below the coarsest backed level until the final
// level fits in one stored tile. Existing backed levels are never changed and
// repeated calls are idempotent.
void appendGeneratedRasterCoverageLevels(std::vector<RasterLevel> &levels,
                                         std::uint32_t baseWidth,
                                         std::uint32_t baseHeight);

[[nodiscard]] std::size_t
rasterBackedLevelCount(std::span<const RasterLevel> levels) noexcept;
[[nodiscard]] std::size_t
rasterGeneratedLevelCount(std::span<const RasterLevel> levels) noexcept;

[[nodiscard]] RasterLayerStyle
defaultRasterLayerStyle(const RasterLayerMetadata &metadata);
[[nodiscard]] RasterLayerStyle clampRasterLayerStyle(RasterLayerStyle style);

// Scene-facing bounds: the raster's XY footprint at the styled elevation. Only
// the zero-thickness Z dimension is inflated, so scene fitting has something to
// frame while the quad still draws at exactly the configured Z.
[[nodiscard]] Bounds3d
rasterSceneBounds(const RasterLayerMetadata &metadata,
                  const RasterLayerStyle &style,
                  RasterElevationStatus elevationStatus =
                      RasterElevationStatus::NotApplicable,
                  std::optional<RasterElevationRange> exactRange =
                      std::nullopt) noexcept;

[[nodiscard]] RasterRenderMode
rasterEffectiveRenderMode(const RasterLayerMetadata &metadata,
                          const RasterLayerStyle &style,
                          RasterElevationStatus elevationStatus,
                          const std::optional<RasterElevationRange> &exactRange)
    noexcept;

// True when a style change alters decoded pixels and must therefore invalidate
// cached tiles. Opacity and elevation are shader uniforms and do not.
[[nodiscard]] bool
rasterDecodeAffectedBy(const RasterLayerStyle &before,
                       const RasterLayerStyle &after) noexcept;

// Affine transform. Coefficients follow GDAL exactly:
//   worldX = gt[0] + pixel * gt[1] + line * gt[2]
//   worldY = gt[3] + pixel * gt[4] + line * gt[5]
// Geometry always uses pixel *edges*, never centers.
[[nodiscard]] bool
rasterAffineInvertible(const std::array<double, 6> &geoTransform) noexcept;

[[nodiscard]] Vec3d
rasterPixelToWorld(const std::array<double, 6> &geoTransform,
                   double pixel,
                   double line) noexcept;

struct RasterPixelCoordinate {
    double pixel = 0.0;
    double line = 0.0;
    bool operator==(const RasterPixelCoordinate &) const = default;
};

[[nodiscard]] std::optional<RasterPixelCoordinate>
rasterWorldToPixel(const std::array<double, 6> &geoTransform,
                   double worldX,
                   double worldY) noexcept;

// The four pixel-edge corners in (0,0), (W,0), (W,H), (0,H) order.
[[nodiscard]] std::array<Vec3d, 4>
rasterCornerPoints(const std::array<double, 6> &geoTransform,
                   std::uint32_t width,
                   std::uint32_t height) noexcept;

[[nodiscard]] std::optional<Bounds3d>
rasterPixelEdgeBounds(const std::array<double, 6> &geoTransform,
                      std::uint32_t width,
                      std::uint32_t height) noexcept;

// A non-reprojected geographic raster whose transformed X extent exceeds 180
// degrees has wrapped: its min/max box spans the wrong side of the globe and
// would corrupt scene fitting for every other layer.
[[nodiscard]] bool rasterCrossesAntimeridian(const Bounds3d &bounds,
                                             bool geographicCrs) noexcept;

// Level-table validity. Entry 0 must be the base dimensions; every later entry
// must be non-increasing in both dimensions and strictly smaller in at least
// one, so duplicates and non-decreasing entries are rejected.
[[nodiscard]] bool rasterLevelTableValid(std::span<const RasterLevel> levels,
                                         std::uint32_t baseWidth,
                                         std::uint32_t baseHeight) noexcept;

[[nodiscard]] std::uint32_t rasterLevelTileCountX(const RasterLevel &) noexcept;
[[nodiscard]] std::uint32_t rasterLevelTileCountY(const RasterLevel &) noexcept;

// levelIndex indexes the inspected RasterLevel table. It never implies a
// power-of-two reduction, so ancestry and child enumeration are geometric.
struct RasterTileKey {
    std::uint32_t levelIndex = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    bool operator==(const RasterTileKey &) const = default;
    auto operator<=>(const RasterTileKey &) const = default;
};

// Inner (non-gutter) valid extent of one tile, which is shorter than
// rasterTilePixels for the level's last row and column. Zero when the key
// falls outside the level.
struct RasterTileExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const RasterTileExtent &) const = default;
};

[[nodiscard]] RasterTileExtent rasterTileValidExtent(const RasterLevel &,
                                                     RasterTileKey) noexcept;

struct RasterBasePixelRect {
    double minimumPixel = 0.0;
    double minimumLine = 0.0;
    double maximumPixel = 0.0;
    double maximumLine = 0.0;
    bool operator==(const RasterBasePixelRect &) const = default;
    [[nodiscard]] bool overlaps(const RasterBasePixelRect &) const noexcept;
};

// A tile's extent in base-pixel coordinates, which is the common frame in
// which levels with arbitrary reduction ratios are compared. The level's last
// row and column are snapped to the base dimensions so adjacent tiles share an
// identical edge and the outer boundary has no floating-point gap.
[[nodiscard]] RasterBasePixelRect
rasterTileBasePixelRect(const RasterLevel &level,
                        RasterTileKey key,
                        std::uint32_t baseWidth,
                        std::uint32_t baseHeight) noexcept;

} // namespace pci

template <> struct std::hash<pci::RasterTileKey> {
    [[nodiscard]] std::size_t
    operator()(const pci::RasterTileKey &key) const noexcept;
};
