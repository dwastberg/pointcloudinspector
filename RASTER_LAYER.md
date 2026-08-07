# GDAL Raster Layers

## Summary

Add georeferenced raster imagery as ordered scene layers rendered on a flat
plane at `z = 0` by default. Multiple selected files become independent
layers; existing GDAL mosaic/catalog datasets such as VRT and GTI open as
single collection layers.

Raster pixels remain on disk until needed. A camera-driven tiled LOD system
requests visible GDAL windows, uses source overviews where available, and
progressively replaces coarse parent tiles with native-resolution tiles when
zooming. GDAL `RasterIO()` supports windowed, resampled reads and
automatically uses suitable overviews; GTI is specifically designed for
spatially indexed catalogs containing hundreds of thousands of rasters. See
the [Raster API](https://gdal.org/en/stable/tutorials/raster_api_tut.html),
[GTI driver](https://gdal.org/en/stable/drivers/raster/gti.html), and
[VRT driver](https://gdal.org/en/stable/drivers/raster/vrt.html)
documentation.

The design rests on three choices, each of which shapes everything below:

- **Delegate mosaicking to GDAL.** The application never builds its own
  catalog, spatial index, or overlap resolution. A VRT or GTI dataset is one
  opaque layer. This removes the hardest part of the problem at the cost of
  depending on how well the user's dataset is authored.
- **Never read more than the screen needs.** Every runtime pixel read is a
  windowed 1:1 `RasterIO()` call against an explicitly selected base/overview
  band and is bounded to one guttered tile. Correctness and performance depend
  on overview availability, which the system measures rather than assumes.
- **Render as a flat, unlit overlay.** Rasters are painter-ordered quads with
  depth testing against the point cloud. They are not draped, not lit, and
  not reprojected.

## Four Load-Bearing Decisions

Four decisions below the level of the summary shape the contracts and
algorithms in this document. They are recorded here because each rules out an
approach that looks reasonable and is wrong.

- **GTI is a required capability, so GDAL 3.9 is the floor.** The driver was
  introduced in 3.9; permitting older builds would admit configurations that
  cannot satisfy the large-catalog requirement at all. A version floor is
  still not a driver guarantee, so the application probes GTI and the index
  driver a given catalog needs at runtime regardless.
- **Overview levels are not powers of two.** GDAL exposes each overview as a
  band with its own dimensions, and non-power-of-two reductions are valid and
  common. A tile key therefore names an index into an inspected level table,
  never a `2^L` reduction, and every level relationship — ancestry,
  hysteresis, child enumeration — is computed geometrically from measured
  ratios.
- **Approximate statistics are not assumed cheap.** `GetStatistics(TRUE,
  FALSE)` returns cached statistics when they exist and declines otherwise; it
  is not a promise of a fast scan. When it declines, the application performs
  its own strictly bounded window sample. The forcing form is never called
  during import.
- **Visible selection never walks from a dataset-wide root.** The camera
  frustum is clipped against the finite raster plane and mapped back through
  the inverse geotransform, and tile ranges come directly from that visible
  polygon. Planning cost is proportional to visible screen coverage rather
  than to catalog extent, which is what makes the size-independence argument
  hold for a national-scale catalog.

## Scope and Phasing

Deliver in three independently shippable phases. Each phase leaves the
application in a releasable state, and each de-risks the next.

- **Phase 1 — Static raster.** One file per layer, read into a texture capped
  at 4096x4096 and at the QRhi-reported maximum texture size. The read uses an
  explicitly backed overview when one fits the cap, and otherwise a bounded
  base-band decimating read (see below). Correct georeferenced placement,
  document ordering, visibility, isolation, framing, opacity, elevation offset,
  warnings, and inspector rows are included. A source that qualifies for
  neither path is admitted as metadata but reports "tiled rendering required"
  instead of scanning the base image. This phase proves the scene-model and
  renderer integration without creating an unsafe throwaway full-image read
  path.
- **Phase 2 — Tiled LOD.** Quadtree traversal, tile cache with eviction,
  cancellation, parent fallback, upload throttling, and diagnostics. Replaces
  the phase 1 single-texture path.
- **Phase 3 — Catalogs and scale.** VRT/GTI support, common overview-level
  inspection, driver availability reporting, and the large-source acceptance
  test.

The scene-model change in phase 1 is the widest edit in the project: adding a
third alternative to `SceneLayer::payload` requires updating every visit site
across `src/scene`, `src/renderer/planning`, `src/renderer/rhi`, and
`src/app`. Doing that while streaming code is also in flight is the main
avoidable risk in this work.

## File Impact Map

The following names are the intended implementation layout. Keeping the GDAL
adapter, portable planning/cache code, and QRhi code in separate targets is a
required architecture boundary, not a naming suggestion.

### Files to add

```text
src/raster/RasterLayer.h
src/raster/RasterLayer.cpp
src/raster/RasterTileSource.h
src/raster/RasterTileCache.h
src/raster/RasterTileCache.cpp

src/import/RasterImport.h
src/import/RasterLoadController.h
src/import/RasterLoadController.cpp
src/import/gdal/GdalRuntime.h
src/import/gdal/GdalRuntime.cpp
src/import/gdal/GdalRasterLoader.h
src/import/gdal/GdalRasterLoader.cpp
src/import/gdal/GdalRasterSource.h
src/import/gdal/GdalRasterSource.cpp

src/renderer/rhi/RasterTileStreamer.h
src/renderer/rhi/RasterTileStreamer.cpp
src/renderer/rhi/RasterLayerRenderer.h
src/renderer/rhi/RasterLayerRenderer.cpp
src/renderer/planning/RasterLodPlanner.h
src/renderer/planning/RasterLodPlanner.cpp
shaders/raster.vert
shaders/raster.frag

tests/fixtures/GdalRasterFixtureFactory.h
tests/fixtures/GdalRasterFixtureFactory.cpp
tests/component/GdalRasterImportTests.cpp
tests/qt/RasterLoadControllerTests.cpp
tests/unit/RasterLayerTests.cpp
tests/unit/RasterLodPlannerTests.cpp
tests/unit/RasterTileCacheTests.cpp
tests/qt/RasterLayerRendererTests.cpp
tests/gpu/RasterRenderGpuTests.cpp
```

Responsibilities are deliberately narrow:

- `RasterLayer.*` owns immutable metadata, styles, color/display rules, affine
  helpers, and scene-facing projections.
- `RasterTileSource.h` is the only contract through which non-GDAL code reads
  pixels. It contains no Qt, GDAL, or QRhi types.
- `renderer/planning/RasterLodPlanner.*` is a pure CPU planner accepting
  `FrameCamera`, layer metadata/style, prior selection, and cache capacity,
  and returning selected, fallback, and requested tile keys. It belongs in
  renderer planning because `pcinspector_raster` cannot depend back on scene
  or renderer types.
- `RasterTileCache.*` is a portable byte-accounted LRU for decoded CPU tiles.
- `GdalRasterLoader.*` performs inspection/band selection/range sampling;
  `GdalRasterSource.*` owns the handle pool and tile reads. Together with
  `GdalRuntime.*`, they are the sole raster adapter files and the only
  production code that includes GDAL raster headers.
- `RasterTileStreamer.*` owns request generations, worker scheduling,
  cancellation, decoded-cache admission, and render-thread wakeups. It never
  creates QRhi resources on a worker.
- `RasterLayerRenderer.*` owns QRhi textures, samplers, bindings, pipelines,
  GPU LRU state, uploads, and draw recording.

### Existing files to change

| Area | Files | Required change |
|---|---|---|
| Build | `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt` | Require GDAL 3.9; add raster/import/renderer targets, fixtures, shaders, and tests. |
| Boundaries | `cmake/PciArchitecture.cmake`, `cmake/CheckIncludeBoundaries.cmake`, `cmake/PciShaders.cmake` | Declare allowed target edges, permit GDAL headers only in `src/import/gdal` and fixture/component tests, and package raster shaders. |
| Shared GDAL runtime | `src/import/ogr/OgrRuntime.*`, `src/import/ogr/OgrVectorLoader.cpp` | Move registration/error capture into `import/gdal/GdalRuntime.*`; make OGR and raster adapters share it. Remove the old runtime files after call sites move. |
| Scene | `src/scene/SceneDocument.h/.cpp`, `src/scene/SceneDocumentSnapshot.h/.cpp` | Add raster payload/projections/revision/counts, style mutation, bounds, snapshot accessors, removal, and visibility behavior. Replace assumptions that every non-point layer is a vector. |
| Import jobs | `src/import/ImportServices.h`, `src/import/LoadJobKey.h`, `src/import/LoadJobRow.*` | Own the raster controller, add `LoadJobKind::Raster`, and keep cancellation/retry/task rows exhaustive. |
| Session | `src/app/ApplicationBootstrap.cpp`, `src/app/SceneSession.h/.cpp` | Construct/connect the raster controller, pass target CRS/extent, add loaded sources to the document, merge task rows, and shut down workers before GDAL runtime teardown. |
| Main UI | `src/app/MainWindow.h/.cpp`, `src/app/ToolbarIcons.*` | Add the import action, multi-file picker, capability/error handling, and icon. |
| Layer UI | `src/app/SceneLayerListModel.*`, `src/app/SceneLayersDock.*`, `src/app/LayerInspectorDock.*` | Project raster rows, warnings, metadata, opacity/Z controls, display range, and color-ramp selection. |
| Settings | `src/app/PerformanceSettings.h`, `src/app/PerformanceSettingsStore.cpp`, `src/app/SettingsDialog.h/.cpp`, `src/app/ApplicationConfig.*`, `src/app/ApplicationOptions.*`, `src/app/MemoryBudgetPolicy.*` | Add CPU/GPU/GDAL raster budgets and worker count, persistence, validation, CLI options, and automatic-memory accounting. |
| Frustum geometry | `src/renderer/planning/FrustumCuller.h/.cpp` | Add convex-polygon clipping against the six frustum planes. `planes_` is private today and the only public query is `intersects(const Bounds3d &)`, which cannot express the visible-plane derivation the LOD planner requires. |
| Renderer API | `src/renderer/RenderViewport.h`, `src/renderer/RenderMetrics.h` | Carry raster cache settings and expose raster counters without leaking QRhi/GDAL types. |
| Renderer | `src/renderer/rhi/RenderViewportWidget.cpp`, `src/renderer/rhi/RenderViewportWidget_p.h` | Own the planner/streamer/renderer, reconcile raster generations, upload before a pass, record raster draws in both EDL paths, wake on completion, and release in safe order. |
| Diagnostics | `src/app/RenderDiagnosticsFormatter.*`, `src/app/DiagnosticsDock.*`, `src/app/QualificationReporter.*` | Report raster/GDAL cache use, requests, failures, selected levels, driver availability, and qualification data. |
| Documentation/packaging | `README.md`, `.github/ci-environment.yml`, `.github/package-environment*.yml`, `packaging/flatpak/io.github.dwastberg.PointCloudInspector.yml` | State the feature and GDAL floor, ensure a GTI-capable build, and document optional GPKG catalog support. |
| Existing tests | Scene, UI, renderer contract, settings, diagnostics, qualification, and architecture test sources | Add the raster alternative to every exhaustive expectation and preserve point/vector characterization. |

`SceneLayersDock` may need no behavioral code if it remains fully model-driven,
but it must still be inspected and covered by characterization tests. Likewise,
do not mechanically add raster handling to point-only systems such as
`PointPicker`; document and test that those systems intentionally ignore it.

### Target dependency shape

```text
pcinspector_raster
  -> foundation, pointcloud (CPT stop tables only)

pcinspector_scene
  -> raster

pcinspector_renderer_planning
  -> raster, scene

pcinspector_gdal_runtime
  -> GDAL::GDAL

pcinspector_import_gdal
  -> raster, pcinspector_gdal_runtime

pcinspector_import_ogr
  -> vector, pcinspector_gdal_runtime

pcinspector_import_async
  -> import_api, raster, tasking, Qt6::Core

pcinspector_renderer
  -> raster, scene, renderer_planning, Qt6::GuiPrivate
```

The executable constructs `GdalRasterLoader` but neither the scene nor the
renderer links GDAL. `GDAL::GDAL` belongs on the shared runtime target and
raster/OGR adapters reach it through that boundary. A direct GDAL dependency
from scene, raster-domain, async-controller, app, or renderer targets is
wrong.

## Public Model and Import

- Add a raster module containing:
  - `RasterLayerMetadata`: source path/connection, driver, dimensions, bands,
    affine geotransform, CRS WKT, transformed XY bounds, native pixel size,
    per-level overview dimensions, and CRS/disjoint warnings.
  - `RasterLayerStyle`: `opacity = 1.0`, `zOffset = 0.0`, and for
    single-band continuous sources a display range and color ramp selection.
  - `RasterLevel`: actual width, height, X/Y reduction relative to the base,
    and the GDAL overview index used for each selected color/alpha/mask band.
    Entry 0 is the full-resolution band; later entries are ordered from finer
    to coarser resolution.
  - `RasterTileKey { levelIndex, x, y }`, where `levelIndex` indexes the
    inspected `RasterLevel` table. It never implies a power-of-two reduction.
  - `RasterTileRequest`, decoded premultiplied `RasterTileData`, and a
    GDAL-independent `RasterTileSource::readTile(request, stopToken)`
    interface.
- Extend `SceneLayerKind` and `SceneLayer::payload` with `Raster`, add raster
  projections/count/access/style APIs, and add a `rasterRevision` to
  snapshots alongside the existing `pointRevision` and `vectorRevision`.
  Include raster bounds in scene fitting, visibility, isolation, ordering,
  and removal; inflate only the bounds' zero-thickness Z dimension while
  drawing at the exact configured Z.
- Add `RasterLoadController` and `LoadJobKind::Raster` using the existing
  scheduler/job-row conventions established by `VectorLoadController`.
  Metadata inspection is asynchronous and reads no full-resolution pixel
  payload.
- Implement `GdalRasterSource` with
  `GDALOpenEx(..., GDAL_OF_RASTER | GDAL_OF_READONLY)`. Validate finite
  dimensions, a nonsingular affine geotransform, and at least one displayable
  band. Accept rotated/skewed transforms and compute bounds from all four
  pixel-edge corners.
- File import behavior:
  - Multi-selection creates one raster layer/job per selected dataset,
    preserving successful loads if another fails.
  - VRT, GTI, `.gti.gpkg`, and other GDAL-readable mosaic datasets remain one
    logical layer; the app does not enumerate their member images or build
    its own mosaic.
  - Offer common imagery extensions in the picker while retaining "All files"
    and letting GDAL determine actual compatibility. The offered extension
    list is advisory only and must not gate what GDAL will accept.
- Do not reproject. Compare against the first available point-cloud CRS,
  otherwise the first raster CRS; flag mismatches and initially hide clearly
  XY-disjoint rasters, reusing the existing "Show anyway" affordance already
  provided for vector layers. Missing CRS is a warning, while missing
  geotransformation is an import error.
- Detect antimeridian crossing and set `crossesAntimeridian`. For a geographic
  CRS, a transformed X extent wider than 180 degrees indicates the bounds have
  wrapped; a simple min/max XY box then spans the wrong side of the globe and
  would frame the entire world. Treat it as an import error naming the
  condition rather than loading a layer whose bounds corrupt scene fitting for
  every other layer. Cover it with a fixture; without one this is an
  observation rather than a behavior.
- Raster import is a menu and toolbar action only, with no command-line file
  arguments. This matches vector import; point clouds remain the only
  positional CLI inputs.

## Concrete C++ Contracts

The following sketches define ownership and call direction. They are not
intended to be pasted without tests, but their signatures and invariants
should remain stable unless implementation reveals a concrete conflict.

### Portable raster types

```cpp
// Split between RasterLayer.h (metadata/display/level types) and
// RasterTileSource.h (tile request/data/source types).
namespace pci {

inline constexpr std::uint32_t rasterTilePixels = 256;
inline constexpr std::uint32_t rasterTileGutter = 1;
inline constexpr std::uint32_t rasterStoredTilePixels = 258;
// Above this base-image size, a decimating read of the base band is no longer
// bounded and only an explicitly backed overview may be used. See
// "Bounded base-band reads".
inline constexpr std::uint64_t rasterBoundedBaseReadPixels = 64ULL << 20;
using RasterSourceId = StrongId<struct RasterSourceIdTag>;
struct RasterDecodeParameters;
struct RasterLayerMetadata;

enum class RasterSampleKind : std::uint8_t {
    ContinuousColor,
    ContinuousScalar,
    Categorical,
};

struct RasterBandRef {
    int band = 0;                  // GDAL's one-based band number
    int overview = -1;             // -1 means base band
};

struct RasterLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double basePixelsPerTexelX = 1.0;
    double basePixelsPerTexelY = 1.0;
    std::uint8_t channelCount = 0;
    std::array<RasterBandRef, 4> rgbaBands{};
    std::optional<RasterBandRef> maskBand;
};

struct RasterTileKey {
    std::uint32_t levelIndex = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    bool operator==(const RasterTileKey &) const = default;
};

struct RasterTileRequest {
    RasterTileKey key;
    std::uint64_t renderGeneration = 0;
    std::shared_ptr<const RasterDecodeParameters> decode;
};

struct RasterTileData {
    RasterTileKey key;
    std::uint64_t renderGeneration = 0;
    std::uint16_t validWidth = 0;   // inner pixels, excluding gutter
    std::uint16_t validHeight = 0;
    std::vector<std::byte> rgba;    // always 258 * 258 * 4 bytes

    [[nodiscard]] std::uint64_t byteSize() const noexcept;
};

class RasterTileSource {
public:
    virtual ~RasterTileSource() = default;
    [[nodiscard]] virtual const RasterLayerMetadata &metadata() const noexcept = 0;
    // Throws RasterReadError on failure and RasterReadCancelled when the stop
    // token is requested. It never returns a partially valid tile.
    [[nodiscard]] virtual RasterTileData
    readTile(const RasterTileRequest &, std::stop_token) const = 0;
};

using RasterTileSourcePtr = std::shared_ptr<const RasterTileSource>;
} // namespace pci
```

`readTile()` reports failure by throwing rather than by returning an empty
tile, so that no caller can mistake a zeroed buffer for transparent imagery.
The worker wrapper is the only place that catches: it converts a successful
return into `ReadResult{.tile = ...}`, a `RasterReadError` into
`ReadResult{.error = ...}` for the negative cache, and a
`RasterReadCancelled` into a dropped result that is neither cached nor
reported. Cancellation is not a failure and must not enter the negative cache,
or a cancelled pan would poison tiles the next frame needs.

Keep the display transform separate from source identity. Opacity and Z only
change uniforms; range/ramp changes alter decoded RGBA and therefore increment
`renderGeneration` so stale worker results cannot enter the cache.
`RasterDecodeParameters` is immutable and shared by all requests in one render
generation, so a ramp is not copied per tile and remains alive until active
workers finish.

A palette color table is deliberately **not** part of `RasterDecodeParameters`
or `RasterLevel`. Neither `channelCount` nor `rgbaBands` can express "one index
band plus a lookup table", and the table is a fixed property of the source
rather than of the user-adjustable display transform. It stays inside
`GdalRasterSource`, which expands indices to RGBA during decode and hands the
portable layers an already-expanded tile. Portable code therefore never sees a
palette at all — `RasterSampleKind::Categorical` tells the renderer to sample
with nearest-neighbor, and nothing more.

```cpp
// src/raster/RasterLayer.h (continued)
struct RasterDisplayRange {
    double minimum = 0.0;
    double maximum = 1.0;
    enum class Origin : std::uint8_t { Metadata, CachedStatistics, Sampled };
    Origin origin = Origin::Metadata;
};

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
};

struct RasterLayerMetadata {
    std::filesystem::path sourcePath;
    std::string sourceDriver;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<double, 6> geoTransform{};
    Bounds3d bounds;
    std::string spatialReferenceWkt;
    std::vector<RasterBandInfo> bands;
    std::vector<RasterLevel> levels;
    RasterDecodeParameters defaultDisplay;
    bool crsMissing = false;
    bool crsMismatch = false;
    bool extentDisjointXY = false;
    bool insufficientOverviews = false;
    bool crossesAntimeridian = false;
};

struct RasterLayerStyle {
    float opacity = 1.0F;
    double zOffset = 0.0;
    std::optional<RasterDisplayRange> displayRange;
    std::string colorRampKey; // empty for RGB; stable CPT key for scalar data
    bool operator==(const RasterLayerStyle &) const = default;
};

struct RasterLayerData {
    RasterSourceId sourceId;
    RasterTileSourcePtr source;
    [[nodiscard]] const RasterLayerMetadata &metadata() const noexcept;
};
using RasterLayerDataPtr = std::shared_ptr<const RasterLayerData>;
```

`RasterLayerMetadata` owns ordinary values only; it must not contain a GDAL
dataset pointer. Its affine coefficients follow GDAL exactly:

```text
worldX = gt[0] + pixel * gt[1] + line * gt[2]
worldY = gt[3] + pixel * gt[4] + line * gt[5]
```

Use pixel **edges**, not centers, for geometry and bounds. For width `W` and
height `H`, transform `(0,0)`, `(W,0)`, `(0,H)`, and `(W,H)`. The inverse is
valid only when `det = gt[1]*gt[5] - gt[2]*gt[4]` is finite and nonzero. Use a
scale-relative epsilon rather than comparing the determinant directly to
machine epsilon.

Assign `RasterSourceId` from a checked monotonic sequence when inspection
creates the immutable source. It distinguishes cache ownership without making
the portable raster target depend on `SceneLayerId`, which would introduce a
scene/raster target cycle. A GPU record may additionally store `SceneLayerId`
for layer pruning, but that renderer-only identity is not part of the decoded
cache key.

### Scene integration

```cpp
enum class SceneLayerKind : std::uint8_t {
    None = 0,
    PointCloud = 1,
    Vector = 2,
    Raster = 3,
};

struct RasterLayerState {
    RasterLayerDataPtr data;
    RasterLayerStyle style;
    std::uint64_t renderGeneration = 1;
};

struct RasterLayer {
    SceneLayerId id;
    RasterLayerDataPtr data;
    bool visible = true;
    RasterLayerStyle style;
    std::uint64_t renderGeneration = 1;
};

struct SceneLayer {
    SceneLayerId id;
    bool visible = true;
    std::variant<PointCloudLayerState, VectorLayerState, RasterLayerState>
        payload;
};
```

Add these `SceneDocument` methods and matching snapshot projections:

```cpp
[[nodiscard]] SceneLayerId addRasterLayer(RasterLayerDataPtr data,
                                          bool initiallyVisible = true);
[[nodiscard]] std::optional<RasterLayer> rasterLayer(SceneLayerId) const;
[[nodiscard]] std::vector<RasterLayer> rasterLayers() const;
[[nodiscard]] std::size_t rasterLayerCount() const noexcept;
[[nodiscard]] bool setRasterLayerStyle(SceneLayerId, RasterLayerStyle);
```

`addRasterLayer()` validates the source and metadata, initializes style from
`metadata().defaultDisplay`, assigns visibility, and calls `markRasterChanged()`.
The source ID must be unique among attached raster data. Reject a duplicate
`RasterLayerDataPtr` just as point scenes reject duplicate attachment.

`setRasterLayerStyle()` increments `renderGeneration` only when display range
or ramp changes; opacity/Z changes increment `rasterRevision` but retain
decoded pixels. Z changes also alter bounds. Audit every existing fallback of
the form "if not point, it is vector"—after this change that is a bad variant
access, not merely a missing feature.

Preserve existing count semantics: `layerCount()` remains the point-cloud
count for compatibility, while `vectorLayerCount()` and the new
`rasterLayerCount()` each count their own variant explicitly. In particular,
`SceneDocumentSnapshot::vectorLayerCount()` can no longer return
`layers.size() - layerCount()`. Replace `copyVectorLayersFrom()` with
`copyOverlayLayersFrom()` (or add a paired raster copy) so replacing a batch
of point clouds preserves both vector and raster overlays in their relative
document order.

Copying an overlay retains its immutable source pointer and style and
**preserves its `SceneLayerId`**, matching what `copyVectorLayersFrom()` does
today: it pushes the source layer whole and advances `nextLayerValue_` past the
copied ID. Do not reassign IDs during the copy. `VectorLayerRenderer::layers_`
is keyed by `SceneLayerId` and prunes against the document, so fresh IDs would
evict and re-upload all vector geometry on every point-cloud replacement — a
performance regression in existing functionality caused by a raster change. The
new raster GPU cache inherits the same constraint. Both production callers are
point-cloud replacement paths in `SceneSession.cpp`.

### Import contract and controller

```cpp
// src/import/RasterImport.h
struct RasterImportRequest {
    std::filesystem::path sourcePath;
    std::string targetSpatialReferenceWkt;
    std::optional<Bounds3d> targetExtent;
    std::stop_token stopToken;
};

struct RasterImportPreflight {
    RasterLayerDataPtr data;
};

class RasterLoader {
public:
    virtual ~RasterLoader() = default;
    [[nodiscard]] virtual RasterImportPreflight
    inspect(const RasterImportRequest &) const = 0;
};
```

The controller has a short state machine:

```text
Queued -> Inspecting -> SamplingRange (only when needed) -> Ready
                   \-> Failed
any nonterminal state -> Cancelled
```

Its primary signal should transfer the already-inspected source without
reopening it on the UI thread:

```cpp
void loaded(pci::LoadJobId,
            pci::RasterLayerDataPtr,
            bool initiallyVisible);
```

`SceneSession` connects that signal to `document_->addRasterLayer()`, publishes
the snapshot, and frames visible layers only for the first layer in an empty
document. Each selected path is a separate job and task row. Do not use the
full-window loading overlay for additive raster imports; show the Tasks dock,
matching additive vector behavior.

### GDAL inspection outline

```cpp
RasterImportPreflight
GdalRasterLoader::inspect(const RasterImportRequest &request) const
{
    checkCancelled(request.stopToken);
    DatasetPtr dataset = openRaster(request.sourcePath);
    requirePositiveDimensions(*dataset);

    std::array<double, 6> gt{};
    if (dataset->GetGeoTransform(gt.data()) != CE_None ||
        !validAffine(gt)) {
        throw RasterImportError("Raster has no usable affine geotransform");
    }

    RasterBandSelection selection = selectDisplayBands(*dataset);
    std::vector<RasterLevel> levels =
        intersectBackedLevels(*dataset, selection);
    if (levels.empty())
        throw RasterImportError("Raster has no common readable band level");

    RasterLayerMetadata metadata = buildMetadata(
        *dataset, request.sourcePath, gt, selection, levels);
    metadata.crsMismatch = crsDiffers(metadata.spatialReferenceWkt,
                                      request.targetSpatialReferenceWkt);
    metadata.extentDisjointXY = request.targetExtent &&
        xyDisjoint(metadata.bounds, *request.targetExtent);

    RasterDecodeParameters display = inspectOrSampleDisplayRange(
        *dataset, selection, levels, request.stopToken);
    metadata.defaultDisplay = std::move(display);
    auto source = std::make_shared<GdalRasterSource>(
        request.sourcePath, metadata, selection);
    auto data = std::make_shared<RasterLayerData>(RasterLayerData{
        .sourceId = nextRasterSourceId(),
        .source = std::move(source),
    });
    return {.data = std::move(data)};
}
```

`intersectBackedLevels()` must match levels by `(width,height)`, not overview
array index. Drivers may expose different ordering or omit an overview on one
band. When a mask is required, include it in the intersection. Preserve the
per-band overview index in `RasterLevel` so `readTile()` never asks GDAL to
choose an overview implicitly.

### GDAL tile read outline

```cpp
RasterTileData GdalRasterSource::readTile(
    const RasterTileRequest &request, std::stop_token stop) const
{
    HandleLease lease = handles_.acquire(stop); // exclusive dataset handle
    const RasterLevel &level = metadata_.levels.at(request.key.levelIndex);
    const Window window = tileWindow(level, request.key, rasterTileGutter);

    ScratchReservation reservation =
        scratchBudget_->reserveOrThrow(requiredScratchBytes(window));
    ChannelPlanes planes = readSelectedBands1To1(
        *lease, level, window.clipped, stop);
    RasterTileData tile = expandPremultipliedRgba(
        request, window, planes, *request.decode);
    replicateOuterGutter(tile, window);
    return tile;
}
```

Use `GDALRasterIOExtraArg` with a progress callback that returns false after
the stop token is requested. Check cancellation between band reads as well;
not every driver calls the progress callback frequently. Acquire the CPU
scratch reservation before allocating or reading. A worker captures a shared
source, so removing a layer cancels queued work while an active read keeps the
source and handle pool alive until it exits safely.

## Pixel Interpretation and Color

Display fidelity, not scientific analysis, is the goal. The rules below are
chosen so that the formats a point-cloud user actually drags in — orthophotos
and terrain models — look correct without configuration.

- Support natural-color Byte and UInt16 imagery, grayscale,
  grayscale-plus-alpha, RGB/RGBA, palette, dataset masks, alpha bands, and
  nodata transparency.
- **Byte** maps directly.
- **UInt16** must not be mapped through its full 0–65535 range by default.
  Many 16-bit aerial sources use only part of that domain, and a full-range
  map can render them nearly black. Prefer valid band scale/range metadata,
  then cached statistics returned by `GetStatistics(TRUE, FALSE)`. If neither
  is available, sample at most 64 evenly distributed 64x64 windows (262,144
  values per selected band) from the coarsest usable overview and apply a
  2nd–98th percentile stretch. Window sampling avoids turning 262,144
  individual sample locations into 262,144 compressed-block reads.
  Sampling is an import task, observes cancellation, excludes nodata/NaN, and
  never writes PAM sidecars. Expose the chosen range and its provenance
  (`metadata`, `cached statistics`, or `bounded sample`) in the inspector.
  Use one shared display range across RGB bands to avoid silently changing
  color balance. Apply explicit per-band scale/offset first, then derive the
  shared range in that scaled domain.
- **Single-band Float32/Float64** is supported, not rejected. Terrain models
  are the second most common raster in a point-cloud workflow after
  orthophotos, and they are float single-band with no color interpretation.
  Derive the display range using the same non-forcing metadata/statistics and
  bounded-sample policy as UInt16, then render through a continuous color
  ramp. Reuse `parseCptColorMap()` and the
  `PointColorStop` ramp data from `src/pointcloud/CptColorMapParser.h` and
  the embedded `:/colormaps` resources. Do not reuse `PointColorMap` or
  `compatibleSourceMask`, which are point-source-specific; the raster path
  consumes stop tables, not the point catalog's identity types.
- Reject only multispectral datasets with no identifiable RGB or grayscale
  band assignment, and complex data types. Report the reason in the layer
  error, naming the band count and interpretations found.
- Resolve bands deterministically in this order:
  1. Explicit GDAL Red/Green/Blue interpretations, plus an optional Alpha.
  2. One palette-index band with a valid color table.
  3. One Gray band, plus an optional Alpha.
  4. Positional fallback for exactly three or four same-type bands whose
     interpretations are all Undefined; record that fallback as a warning.
  Reject ambiguous mixtures instead of guessing. For a single continuous
  scalar, use the `viridis` CPT ramp by default rather than pretending it is
  photographic grayscale.
- Compose transparency as
  `colorTableAlpha * explicitAlpha * validity`, where validity is zero for
  nodata, NaN scalar samples, or an invalid dataset/per-band mask and one
  otherwise. Convert alpha to `[0,1]`, multiply RGB before quantizing to
  RGBA8, and never apply layer opacity during decode; opacity is a shader
  uniform and must not invalidate cached tiles.
- **Nodata and resampling interact and must be handled explicitly.** Reading
  color with bilinear GDAL resampling, reading a mask separately, and
  premultiplying afterward is insufficient: invalid source colors may already
  have contaminated valid output samples. Restrict runtime tile levels to
  explicitly backed base/overview bands, read their samples 1:1, expand
  color/palette plus alpha/mask on the CPU, and premultiply before the GPU
  performs linear filtering. A level is usable only when every required color
  and alpha/mask band has the same dimensions at that level. This produces a
  common, bounded source window and removes nodata fringes without a custom
  resampler.
- Continuous RGBA textures use a linear QRhi sampler; palette/categorical
  textures use nearest-neighbor. Palette expansion happens during CPU decode,
  after the overview has been selected but before upload.

## Streaming and LOD

- Inspect a level table rather than constructing a synthetic quadtree. Level
  0 is the base band and subsequent levels are the common, explicitly backed
  overview dimensions available to every required band. Reject duplicate or
  non-decreasing entries and record asymmetric X/Y reduction factors.
- Determine visibility without traversing the complete dataset. Start with
  the raster's four world-space corners at `style.zOffset`, clip that polygon
  against all six camera-frustum planes, map the surviving polygon through
  the inverse geotransform, and convert its bounds to tile coordinates at the
  selected level. Test candidate tile quads against the clipped polygon so a
  thin rotated footprint does not fill its much larger axis-aligned bounds.
  Expand the result by one tile for prefetch, then apply the hard selected-tile
  cap.
- Select the coarsest available level whose texel projects to at most 1.25
  screen pixels. Preserve the prior level until either its texel exceeds 1.25
  pixels (move finer) or the next coarser level's texel falls below 0.8 pixels
  (move coarser). Because the coarsen test is made against the candidate level
  rather than the current level, this remains stable for arbitrary overview
  ratios.
- If the desired screen resolution is coarser than the coarsest backed level,
  use that level only while its visible tile count fits the cache/admission
  limit. Otherwise render the subset that fits and flag
  `insufficientOverviews`. Runtime tile reads never fall back to a base-band
  downsample. Close views clamp to level 0, so one texture texel corresponds to
  one native source pixel.

### Bounded base-band reads

The prohibition on base-band downsampling exists because a decimating read of
an unbounded base image is an unbounded scan. It is a statement about size, not
about the base band as such: a decimating read is bounded exactly when the base
is bounded.

Applying it as a blanket rule would make an ordinary un-overviewed 5000x5000
GeoTIFF — the most common thing a user drags in first — undisplayable in
Phase 1, because its level table is exactly `[5000x5000]` and no entry fits the
4096 texture cap.

Permit a single decimating base-band read when
`width * height <= rasterBoundedBaseReadPixels` (64 MPix, roughly 100 MB of
Byte RGB traffic). Above that threshold the prohibition stands and the source
reports `insufficientOverviews` with "tiled rendering required".

The allowance applies only to Phase 1's one-shot static texture and to
inspection-time sampling. Phase 2 runtime tile reads are always 1:1 windows
against an explicitly selected base or overview band, with no decimation,
regardless of source size. Gate 1 and Gate 2 must reference this same constant
so their behavior cannot diverge.
- Read each tile with a one-texel gutter into a 258x258 premultiplied RGBA
  buffer. Reads are 1:1 windows from the selected base/overview band. At the
  dataset's outer border the gutter falls outside valid pixels; clamp the
  read window and replicate the edge texel into the remaining gutter rather
  than letting GDAL error or return fill. Store the inner valid width/height
  for edge-tile UV and geometry calculation.
- Keep a resident coarser ancestor visible until every selected child is
  decoded and uploaded, preventing holes or flashes during refinement.
  Prioritize coarse coverage first, then visible detail by screen-center
  distance; cap queued requests and cancel stale generations after camera,
  visibility, or document changes.
- Use two cancellable raster-read workers in a pool separate from the
  hierarchy `TaskScheduler`, so that a long-running point-cloud import cannot
  starve tile reads and leave the viewport blank. Make the worker count a
  performance setting. `GdalRasterSource` maintains at most one exclusively
  leased read-only GDAL handle per worker, because a dataset handle is never
  accessed concurrently. Note that each additional handle to a VRT or GTI
  dataset opens its own member datasets; keep the worker count low for this
  reason.
- Cache failed tile requests negatively for the current source generation,
  retain a coarser ancestor when possible, and report one actionable layer
  error instead of retrying every frame. Cancellation and layer removal must
  release GDAL handles, decoded tiles, GPU textures, and queued work.

### LOD planner interface and algorithm sketch

LOD is spatially adaptive: an oblique raster can use fine tiles near the
camera and coarse tiles farther away. A single level for the entire layer
would over-read the far field or blur the near field.

```cpp
struct RasterLodPlanInput {
    RasterLayer layer;
    FrameCamera camera;
    // Sorted by level then y then x so a region's prior level can be found by
    // geometric overlap, not key equality. See "Hysteresis across frames".
    std::span<const RasterTileKey> previousSelection;
    std::function<bool(RasterTileKey)> cpuResident;
    std::function<bool(RasterTileKey)> gpuResident;
    // Hard planner ceiling. The effective cap is
    // min(maximumSelectedTiles, gpuCapacityTiles).
    std::size_t maximumSelectedTiles = 512;
    std::size_t gpuCapacityTiles = 512;
};

struct RasterLodPlan {
    std::vector<RasterTileKey> selected;  // target detail
    std::vector<RasterTileKey> draw;      // resident target or ancestors
    std::vector<RasterTileKey> requests;  // priority order
    std::vector<RasterTileKey> protectedTiles;
    bool insufficientOverviews = false;
    bool capacityLimited = false;
};

[[nodiscard]] RasterLodPlan planRasterTiles(const RasterLodPlanInput &);
```

Planner steps:

1. Construct the four finite raster-plane corners at `style.zOffset` and
   clip them against `FrameCamera::culler`'s planes. Return an empty plan if
   the polygon is empty or behind the perspective near plane.

   This requires a new `FrustumCuller` capability. Today `planes_` is private
   and the only public query is `intersects(const Bounds3d &)`, which answers
   a different question. Add clipping to `FrustumCuller` rather than exposing
   the plane array, so the representation stays encapsulated and the
   Sutherland–Hodgman implementation has one home:

   ```cpp
   // src/renderer/planning/FrustumCuller.h
   [[nodiscard]] std::vector<Vec3d>
   clipConvexPolygon(std::span<const Vec3d> polygon) const;
   ```

   The existing `Plane` convention already fixes the sign: `normal` points
   into the visible half-space and
   `signedDistance(p) = dot(normal, p) + distance`, so a vertex is inside when
   that value is `>= 0`. Clipping a convex polygon against six half-spaces
   yields at most `4 + 6` vertices, so the result needs no general-polygon
   machinery. Unit-test it independently of raster code: a quad fully inside,
   fully outside, straddling each plane, and degenerate to a point.
2. Map the clipped polygon through the checked inverse affine. Clamp the
   result to `[0,width] x [0,height]`; never cast a negative or non-finite
   coordinate to an unsigned tile index.
3. At the coarsest common backed level, enumerate only tile cells intersecting
   the clipped polygon. Use checked row spans rather than allocating a bitmap
   proportional to raster dimensions.
4. Put visible cells into a priority queue. For each cell, project its four
   world corners and calculate the largest on-screen length of one source
   texel. If it exceeds 1.25 pixels and a finer backed level exists, replace
   the cell with all overlapping visible cells in that finer level. Retain a
   previous finer selection until the next coarser texel is below 0.8 pixels.
5. Stop before materializing children when their checked count would exceed
   the effective cap, `min(maximumSelectedTiles, gpuCapacityTiles)`. Mark the
   result capacity-limited and keep the parent selected. Arbitrarily large gaps
   between overviews can otherwise turn one parent into millions of child
   candidates. The two inputs are independent limits — a fixed planner ceiling
   and a budget-derived residency ceiling — and the planner always honors the
   smaller.
6. For each selected tile, draw it if GPU-resident. Otherwise walk toward
   coarser levels and choose the first resident overlapping ancestor. Request
   missing coverage ancestors before detail children, then order equal-level
   detail by distance to viewport center.

The "ancestor" relation is geometric, not `x / 2, y / 2`, because adjacent
GDAL levels may have arbitrary ratios. Convert the child's base-pixel extent
to the candidate level and enumerate the overlapping tile cells.

#### Hysteresis across frames

The same geometric treatment is required for the hysteresis state itself, and
for the same reason. `previousSelection` is a flat list of tile keys, but a
region that was drawn at level 3 last frame may be under consideration at level
2 this frame, where no key from the previous list matches. Looking up the prior
level by key equality would therefore miss on most frames during a zoom, and
hysteresis would silently degenerate to none — precisely the flapping the
candidate-level rule exists to prevent, reintroduced through the state lookup.

Define `priorLevelFor(cell)` as: convert the candidate cell's extent to base
pixels, find the previously selected tiles whose base-pixel extents overlap it,
and take the finest level among them. Absent any overlap, the region is new and
has no prior level, so the plain "coarsest level at or under 1.25 px" rule
applies. Keeping `previousSelection` sorted by level then row then column makes
this a bounded range scan rather than a full pass.

Test it directly: hold the camera still across two frames with a synthetic
non-power-of-two level table and assert the selection is identical; then sweep
slowly across a boundary and assert no cell changes level twice in the same
direction-free interval.

### Streaming state machine

Only the render thread mutates tile state. Workers return immutable results
through the same queued-callback pattern used by `SceneSnapshotCache` and
load controllers.

```cpp
enum class RasterTileState : std::uint8_t {
    Missing,
    Queued,
    Reading,
    CpuResident,
    GpuResident,
    Failed,
};

struct RasterCacheKey {
    RasterSourceId sourceId;
    std::uint64_t renderGeneration;
    RasterTileKey tile;
    bool operator==(const RasterCacheKey &) const = default;
};
```

```cpp
void RasterTileStreamer::reconcile(const RasterLodPlan &plan,
                                   const RasterLayer &layer)
{
    ++requestEpoch_;
    cancelQueuedKeysNotIn(plan.requests);
    cpuCache_.protect(plan.protectedTiles);

    for (const RasterTileKey key : plan.requests) {
        RasterCacheKey cacheKey{
            layer.data->sourceId, layer.renderGeneration, key};
        if (knownOrInFlight(cacheKey))
            continue;
        if (!reserveRequestSlot(cacheKey))
            break;
        scheduleRead(layer, cacheKey, requestEpoch_);
    }
}

void RasterTileStreamer::accept(ReadResult result)
{
    // Executed on the QObject/render thread.
    releaseInFlightReservation(result.key);
    if (result.epoch != requestEpoch_ || !layerStillMatches(result.key))
        return; // stale completion; payload is destroyed without upload
    if (!result.tile) {
        negativeCache_.insert(result.key, result.error);
        wake_();
        return;
    }
    cpuCache_.insert(result.key, std::move(*result.tile));
    pendingUploads_.push_back(result.key);
    wake_();
}
```

Request epochs cancel obsolete camera work, while `renderGeneration` rejects
results decoded with an obsolete range/ramp. Camera changes must not change
`renderGeneration`; doing so would discard reusable tiles on every frame.
Negative failures are cleared only by retry, source replacement, or a new
render generation—not by camera movement.

## Memory Budgets

Three separate allocators hold raster pixels. Budgeting two of them and
ignoring the third produces metrics that report compliance while process
memory grows without bound.

- Add independently configurable raster cache limits, defaulting to 256 MiB
  CPU and 256 MiB GPU, plus a process-global GDAL block-cache limit defaulting
  to 128 MiB:
  - CPU accounting includes decoded tiles, active reads, and queued upload
    payloads.
  - GPU accounting includes tile textures and bindings.
  - Both caches use LRU eviction while protecting tiles drawn this frame and
    their fallback ancestors.
- **Set GDAL's block cache explicitly.** `GDAL_CACHEMAX` defaults to 5% of
  physical RAM, is process-global, and is invisible to the accounting above —
  on a 64 GB machine that is roughly 3 GB the application neither controls
  nor reports. Call `GDALSetCacheMax64()` during runtime initialization, fold
  the value into the documented total memory envelope, and surface
  `GDALGetCacheUsed64()` in the diagnostics alongside the CPU and GPU byte
  counters.
- **Prevent pins from exceeding the budget.** Compute GPU tile capacity before
  planning, reserve one quarter for fallback ancestors and uploads, and pass
  the remaining capacity to the LOD planner. If viewport coverage would exceed
  it, retain coarser parents. Validate an absolute floor of 16 resident tiles;
  above that floor the configured value remains a hard limit rather than
  being silently exceeded or dynamically clamped after settings validation.
- Limit uploads to 32 MiB per frame and retain at most 256 pending tile keys,
  so source size and catalog cardinality cannot produce unbounded memory or
  queue growth.

All payload allocations use reservations. The decoded cache reserves
`rgba.capacity()` rather than the logical pixel count; an in-flight worker
reserves channel scratch plus the destination tile before calling GDAL; a
queued upload continues holding its decoded-cache reservation until QRhi has
consumed the update. Container nodes, GDAL dataset objects, QRhi binding
objects, and driver-private allocations are reported separately because their
exact allocator sizes are not observable.

```cpp
bool RasterTileCache::makeRoom(std::uint64_t incoming,
                               std::span<const RasterCacheKey> protectedKeys)
{
    if (incoming > byteBudget_)
        return false; // cannot ever fit; caller must skip admission
    while (residentBytes_ > byteBudget_ - incoming) {
        auto victim = leastRecentlyUsedUnprotected(protectedKeys);
        if (victim == entries_.end())
            return false;
        residentBytes_ -= victim->second.accountedBytes;
        entries_.erase(victim);
        ++evictions_;
    }
    return true;
}

void RasterTileCache::setByteBudget(
    std::uint64_t bytes, std::span<const RasterCacheKey> protectedKeys)
{
    byteBudget_ = bytes;
    static_cast<void>(makeRoom(0, protectedKeys)); // restore invariant now
}
```

Never write `incoming + residentBytes_ > byteBudget_`; that addition can
overflow. Guard `incoming > byteBudget_` first, then compare against
`byteBudget_ - incoming` — a form whose right-hand side cannot underflow once
that guard has passed.

Note that the naive rearrangement `incoming > byteBudget_ - residentBytes_` is
**not** equivalent and is unsafe here. The settings section requires CPU, GPU,
and GDAL limits to apply live, and lowering a budget is exactly what makes
`residentBytes_ > byteBudget_`. That subtraction would then wrap to a huge
value, the loop would never run, and the insert would proceed over budget with
no assertion failure, because the counters stay internally consistent.

Because a live decrease can leave the cache temporarily over budget, a budget
change must evict down to the new limit immediately rather than waiting for the
next insert; `setByteBudget()` above does that by calling `makeRoom(0, ...)`.
The same treatment applies to the GPU residency cache. Unit-test it: fill to
capacity, lower the budget, and assert resident bytes drop to the new limit
before any further admission.

Use `CheckedArithmetic` for tile counts, row strides, and byte sizes before
narrowing to GDAL or QRhi integer types.

The planner computes
`maximumResidentTiles = gpuBudget / accountedGpuTileBytes`, reserves one
quarter for ancestors/uploads, and uses the rest as the target-tile cap.
Account a conservative fixed 1 KiB per tile for SRB/resource bookkeeping in
addition to texture bytes because QRhi does not expose binding allocation
sizes.

### Boundedness proof

Let `C` be the configured raster CPU budget, `G` the GPU budget, `D` the GDAL
block-cache limit, `Q = 256` pending keys, and `W` the worker count.

- Every decoded resident, in-flight destination, scratch plane, and queued
  upload owns a reservation before allocation. Their combined accounted
  payload is therefore at most `C`; `W` cannot multiply that ceiling.
- GPU admission evicts before creation and the planner bounds protected tiles,
  so accounted texture plus conservative binding bytes are at most `G`.
- GDAL's shared block cache is at most `D` after `GDALSetCacheMax64(D)`.
- Pending work not yet reading holds only `Q` fixed-size keys/state records,
  not pixel buffers.
- The documented pixel-memory envelope is consequently `C + G + D`, plus
  bounded metadata, dataset-handle, allocator, command-buffer, and container
  overhead. Qualification tests compare both accounted peaks and process RSS
  because driver-private memory can violate the practical envelope without
  violating these counters.

### Short correctness arguments

- **Native-detail guarantee:** level 0 reads a 1:1 base-band window and one
  decoded texel represents one source pixel. The planner cannot refine beyond
  level 0. Magnification may interpolate those native texels, but it never
  magnifies a previously downsampled preview.
- **Affine placement:** every tile edge is expressed in base pixel-edge
  coordinates and passed through the same six-term affine as the full raster.
  Adjacent tiles share an identical base-pixel edge, so their world geometry
  has no mathematical gap before float narrowing; eye-relative conversion
  keeps that narrowing stable at large projected coordinates.
- **Catalog-size independence:** the application asks GDAL for dataset
  metadata and visible windows only. Its planner stores at most the selected
  cap plus pending/fallback keys and never loads member paths from VRT/GTI.
  Application memory and planning work are therefore independent of member
  pixel count; GDAL/driver metadata remains the measured external term.
- **No stale upload:** a result enters the CPU cache only if source ID, render
  generation, request epoch, and retained scene layer still match. Removing a
  layer or changing its color transform makes every older completion fail at
  least one check before QRhi upload.

## Rendering

- Add a `RasterLayerRenderer` using fixed-size QRhi RGBA textures and
  per-tile quads derived from the affine transform. Compute tile corners
  relative to the camera eye before converting to floats, matching the
  eye-relative convention the point and vector renderers already use.
- Render raster layers in document order after the point/EDL composite and
  before vector overlays. This means hooking both recording paths in
  `RenderViewportWidget::recordScene()`: when EDL is inactive, overlays
  record into the point target; when EDL is active, they record into the
  widget target after the composite republishes depth.
- Use premultiplied alpha, depth testing, and no depth writes, so that later
  raster layers provide painter ordering and vectors remain the top overlay.
- **Apply a small depth bias to raster geometry.** Depth-test-without-write
  keeps points at the same Z visible only if point depth and raster depth
  agree bit-for-bit at `z = 0`. They will not: point depth comes from a
  point-sprite vertex path and raster depth from an interpolated quad,
  through different transforms with different rounding, and the EDL composite
  adds a further clear-and-republish step. Without a bias the shared plane
  produces moiré that varies by driver and camera angle. Bias the raster
  slightly away from the camera and verify stability across a camera sweep,
  not a single frame.
- Rasters receive no eye-dome lighting. EDL is computed from point depth
  only, so a raster placed above the point cloud shows an unlit surface with
  a hard seam against shaded points. This is accepted for this phase and
  documented in the README rather than worked around.
- Rasters are not pickable and do not participate in measurement. `PointPicker`
  and `MeasurementController` continue to consider point geometry only; a
  click on a raster with no point behind it produces no pick result.

### Renderer data and shader sketch

Use one immutable unit-quad vertex buffer, one shared sampler per filtering
mode, a dynamic uniform buffer, and one texture/SRB pair per resident tile.
The GPU cache key includes layer ID, render generation, and tile key.

```cpp
struct alignas(16) RasterTileUniform {
    std::array<float, 16> mvp{};
    std::array<float, 4> uvRect{}; // minU, minV, maxU, maxV inside gutter
    float opacity = 1.0F;
    std::array<float, 3> padding{};
};
static_assert(sizeof(RasterTileUniform) == 96);

struct RasterTileDraw {
    RasterCacheKey key;
    RasterTileUniform uniform;
    std::uint32_t uniformIndex = 0;
};

class RasterLayerRenderer {
public:
    void ensureResources(QRhi *, QRhiRenderPassDescriptor *);
    std::size_t uploadPending(QRhiCommandBuffer *,
                              std::span<const RasterPendingUpload>,
                              std::span<const RasterCacheKey> protectedKeys,
                              std::uint64_t frameByteBudget);
    void updateUniforms(QRhiCommandBuffer *,
                        std::span<const RasterTileDraw>);
    void recordDraws(QRhiCommandBuffer *, QRhiRenderTarget *,
                     std::span<const RasterTileDraw>);
    void retainLayers(std::span<const SceneLayerId>);
    void releaseResources();
};
```

For an edge tile, UVs cover only its replicated-gutter interior:

```cpp
const float stored = static_cast<float>(rasterStoredTilePixels);
uniform.uvRect = {
    1.0F / stored,
    1.0F / stored,
    (1.0F + tile.validWidth) / stored,
    (1.0F + tile.validHeight) / stored,
};
```

Build the tile world quad from pixel edges at the selected level. Convert
level coordinates to base-pixel coordinates using the inspected X/Y ratios,
apply the affine, subtract `frame.eye`, and only then narrow to float for the
MVP. Force the last tile edge to the base raster width/height to avoid a
floating-point gap at the outer boundary.

```glsl
// shaders/raster.vert
#version 450
layout(location = 0) in vec2 unitPosition;
layout(location = 1) in vec2 unitUv;
layout(location = 0) out vec2 uv;
layout(std140, binding = 0) uniform RasterTileData {
    mat4 mvp;
    vec4 uvRect;
    float opacity;
} tile;
void main()
{
    uv = mix(tile.uvRect.xy, tile.uvRect.zw, unitUv);
    gl_Position = tile.mvp * vec4(unitPosition, 0.0, 1.0);
}
```

```glsl
// shaders/raster.frag
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;
layout(binding = 1) uniform sampler2D imagery;
layout(std140, binding = 0) uniform RasterTileData {
    mat4 mvp;
    vec4 uvRect;
    float opacity;
} tile;
void main()
{
    vec4 premultiplied = texture(imagery, uv);
    fragmentColor = premultiplied * tile.opacity;
}
```

The uniform block must be padded identically in C++ and GLSL and guarded by
`offsetof`/`sizeof` assertions, as `VectorLayerUniform` is today. Verify the
compiled QShader reflection in a renderer contract test.

Configure the raster pipeline with premultiplied blending, depth test enabled,
depth writes disabled, back-face culling disabled, and QRhi depth bias plus
slope-scaled depth bias to push imagery away from the camera. Start with bias
`1` and slope bias `1.0F`; treat those as tested backend parameters, not
universal constants. Draw coarse fallback tiles first and finer target tiles
second; because raster draws do not write depth, the finer tile replaces the
parent by painter order.

Uploads, texture creation, layer pruning, and uniform updates must complete
before `beginPass()`. The existing `recordScene()` ordering becomes:

```text
EDL off: begin widget pass -> points -> rasters -> vectors -> end
EDL on:  begin EDL pass    -> points -> end
         begin widget pass -> EDL composite -> rasters -> vectors -> end
```

Never upload or destroy QRhi resources from a GDAL worker or from a completion
callback that is not executing on the viewport's owning thread.

## Build and Driver Requirements

- **Raise the GDAL floor to 3.9 and still detect drivers at runtime.** GTI was
  introduced in 3.9, so retaining 3.4 would allow supported builds that cannot
  meet the large-catalog requirement. A version floor is still not a complete
  driver guarantee: optional index/storage drivers such as GPKG can be absent
  in a newer build. Update CMake, CI/package environments, README, and packaged
  GDAL data together.
- Probe `GDALGetDriverByName("GTI")` once at startup through the new shared
  runtime helper. Probe `GPKG`, `FlatGeobuf`, or another index driver when the
  selected catalog requires it. A missing GTI driver disables catalog import
  capability; a missing optional index driver produces an import error naming
  that driver rather than a generic open failure. Ordinary GeoTIFF/VRT raster
  import remains available when GTI is absent in an unexpectedly stripped
  build.
- Report the detected GDAL version and catalog-driver availability in the
  qualification output, so a user's bug report distinguishes "GTI missing"
  from "GTI broken."
- Add a configure/CI assertion for `GDAL_VERSION >= 3.9` and a runtime
  capability test. The former protects the supported API baseline; the latter
  verifies the actual packaged drivers.

**The CI dependency has been verified to satisfy this.** All three environment
files (`.github/ci-environment.yml`, `.github/package-environment.yml`,
`.github/package-environment-macos.yml`) pin `libgdal-core`, which is
conda-forge's minimal GDAL build and ships many drivers as separate plugin
packages — so GTI availability could not be assumed. Inspecting the built
library resolves it: `libgdal-core` 3.13.2 exports `GDALRegister_GTI` and
`RegisterOGRGeoPackage` directly from `libgdal.dylib`, its `gdalplugins`
directory contains only `drivers.ini` with no separate driver modules, and the
GPKG, FlatGeobuf, and ESRI Shapefile index formats are all present. GTI is
compiled into core, not packaged as an optional plugin.

Raising the three pins from `>=3.4` to `>=3.9` is therefore sufficient, and
making GTI a hard CI-lane requirement is achievable without adding a
dependency. Re-run the check if the environment files ever move off
`libgdal-core`:

```sh
nm -gU "$PREFIX/lib/libgdal.dylib" | grep GDALRegister_GTI
```

## UI, Diagnostics, and Integration

- Add "Import Raster Layer..." to the File menu and toolbar with multi-file
  selection, following the existing vector import action.
- Represent raster rows alongside point and vector rows with dimensions as
  the summary and warnings for missing/mismatched CRS, disjoint extents, or
  insufficient overviews.
- Add raster inspector controls for opacity and elevation offset, including
  "Place above scene," "Match scene floor," and reset-to-`z = 0`; show
  source, driver, dimensions, bands, native pixel size, overview count and
  coarsest backed level, applied display range, CRS, and bounds. Elevation
  presets resolve against scene bounds at the moment they are applied and do
  not track later scene changes.
- For single-band continuous rasters, add display-range and color-ramp
  controls sharing the embedded CPT ramps.
- Add raster CPU/GPU cache settings, a GDAL block cache setting, and a worker
  count, with corresponding command-line options; persist them with existing
  performance settings and describe that these limits are separate from point
  caches.
- Extend renderer diagnostics with selected/resident/pending tile counts,
  requested/completed/cancelled/failed reads, CPU/GPU raster bytes and peaks,
  GDAL block cache usage, cache hits/evictions, upload bytes, and chosen LOD
  range.
- Add raster library, GDAL import, async import, renderer, shader, test,
  architecture-boundary, packaging, and README entries without exposing GDAL
  types outside the GDAL-backed implementation target.

### Settings and application wiring sketches

```cpp
struct RasterPerformanceSettings {
    std::uint64_t cpuCacheMebibytes = 256;
    std::uint64_t gpuCacheMebibytes = 256;
    std::uint64_t gdalCacheMebibytes = 128;
    std::uint32_t readWorkers = 2;
    bool operator==(const RasterPerformanceSettings &) const = default;
};
```

Expose these as `--raster-cpu-cache-mb`, `--raster-gpu-cache-mb`,
`--gdal-cache-mb`, and `--raster-workers`. CPU/GPU/GDAL limits apply live;
worker-count changes are persisted but explicitly marked "applies after
restart" because the existing `TaskScheduler` worker count is immutable.
Validate all byte conversions before multiplication, constrain workers to
1–8, and enforce minima of 16 MiB CPU, 5 MiB GPU (16 guttered RGBA tiles plus
conservative SRB accounting), and 1 MiB GDAL cache.

Automatic point-memory calculation must subtract the configured raster CPU
and GDAL cache reservations from usable system memory. On unified-memory
platforms, pass the sum of point and raster GPU budgets into the existing GPU
allowance rather than accounting only for point buffers. Otherwise enabling
rasters raises the process envelope without lowering the automatically chosen
point budget.

Bootstrap wiring extends the existing import owner:

```cpp
ImportServices createImportServices()
{
    ImportServices services;
    services.scheduler = std::make_unique<TaskScheduler>();
    services.pointCloud = std::make_unique<PointCloudLoadController>(...);
    services.vector = std::make_unique<VectorLoadController>(...);
    services.raster = std::make_unique<RasterLoadController>(
        std::make_shared<GdalRasterLoader>(), *services.scheduler);
    services.statistics = std::make_shared<PdalPointCloudStatistics>();
    return services;
}
```

Raster tile streaming does **not** use `ImportServices::scheduler`; the
controller uses that scheduler only for inspection and bounded range
sampling. `RasterTileStreamer` owns its separate two-worker scheduler so
interactive imagery is not starved by point imports.

The picker mirrors vector import but starts one independent job per path:

```cpp
void MainWindow::chooseRasterLayers()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Import raster layers"), {},
        tr("Raster files (*.tif *.tiff *.cog *.vrt *.gti *.gpkg *.img "
           "*.jp2 *.png *.jpg *.jpeg);;All files (*)"));
    for (const QString &path : paths) {
        RasterImportRequest request;
        request.sourcePath = qStringToPath(path);
        request.targetSpatialReferenceWkt =
            session_->document()->referenceSpatialReferenceWkt();
        request.targetExtent = session_->document()->visibleSceneBounds();
        session_->startRasterImport(std::move(request));
    }
}
```

Add `SceneDocument::referenceSpatialReferenceWkt()` with deterministic
precedence: first point-cloud layer with a CRS, then first raster layer with a
CRS. It is a comparison aid only and does not imply that the document
reprojects or enforces a CRS. Avoid duplicating this scan in `MainWindow` and
the import controllers.

Session connection follows the vector path:

```cpp
connect(&rasterLoadController(), &RasterLoadController::loaded, this,
        [this](LoadJobId, RasterLayerDataPtr data, bool initiallyVisible) {
            const bool wasEmpty = !document_->hasAnyLayer();
            document_->addRasterLayer(std::move(data), initiallyVisible);
            publishDocument(wasEmpty, false);
        });
```

`publishTaskRows()` concatenates point, vector, and raster rows. `cancelAll`,
keyed cancel/retry/dismiss, `ImportServices::valid()`, and shutdown must include
the raster controller. Destroy controllers before waiting for the shared
import scheduler, as the current owner does.

Viewport wiring should be explicit rather than hidden in
`RasterLayerRenderer`:

```cpp
const RasterFramePlan rasterPlan = rasterCoordinator_.plan(
    *document, currentFrameCamera(), rasterLayerRenderer_.capacity());
rasterTileStreamer_.reconcile(rasterPlan, document->rasterRevision);

// Before beginPass():
auto readyUploads = rasterTileStreamer_.takeReadyUploads(frameUploadBudget);
rasterLayerRenderer_.retainLayers(rasterPlan.retainedLayerIds);
rasterLayerRenderer_.uploadPending(commandBuffer, readyUploads,
                                   rasterPlan.protectedTiles,
                                   frameUploadBudget);
auto rasterDraws = rasterLayerRenderer_.buildDraws(rasterPlan);
rasterLayerRenderer_.updateUniforms(commandBuffer, rasterDraws);
recordScene(commandBuffer, pointDraws, rasterDraws, vectorDraws);
```

When a worker completes, the streamer posts to the viewport thread and invokes
the wake callback, which calls `requestRender()`. Coalesce wakeups with an
atomic flag like `SceneSnapshotCache`; one queued event per tile can otherwise
flood the Qt event loop during a large refinement.

## Implementation Sequence and Exit Gates

Implement in the order below. Do not begin the next gate while the named tests
or invariants are failing.

### Gate 0 — Contracts and GDAL runtime

1. Add `pcinspector_raster`, portable metadata/affine/tile types, and their
   unit tests.
2. Move `OgrRuntime` to shared `GdalRuntime`; preserve thread-local error
   capture and add version/driver/cache functions.
3. Add `pcinspector_import_gdal`, `GdalRasterLoader`, fixture generation, and
   component tests for metadata, band selection, levels, range sampling, and
   one 1:1 tile read.
4. Update architecture checks and require GDAL 3.9 in every supported build.

Exit: point/vector tests are unchanged; no GDAL headers cross the adapter
boundary; component tests demonstrate that inspecting or sampling a huge
logical source performs a bounded number of reads.

### Gate 1 — Scene and static rendering

1. Add the raster scene variant, projections, revisions, bounds, style APIs,
   and reference-CRS helper.
2. Update every variant visit and scene/list/inspector characterization test.
3. Add the controller, session wiring, action, rows, and inspector.
4. Add the raster renderer and static backed-overview texture path, including
   EDL/no-EDL ordering and depth bias.

Exit: one small GeoTIFF and one small Float32 terrain raster load as separate
layers, frame correctly, render at `z = 0`, change opacity/Z, remove cleanly,
and do not regress point picking or vector painter order. An un-overviewed
mid-size GeoTIFF (about 5000x5000, under
`rasterBoundedBaseReadPixels`) renders through the bounded base-band path —
this case must be in the exit set, because a small-fixture-only gate would pass
while the phase remained unusable on ordinary input. Sources above the
threshold with no fitting backed level report "tiled rendering required" and
never scan the base image.

### Gate 2 — Tiled streaming and adaptive LOD

1. Add `FrustumCuller::clipConvexPolygon()` with its own unit tests, then the
   visible-plane clipper and adaptive planner with arbitrary overview ratios,
   geometric cross-frame hysteresis, and the effective tile capacity.
2. Add the separate read scheduler, epochs/generations, negative cache,
   cancellation, CPU LRU, and coalesced wakeups.
3. Add GPU tile residency, upload throttling, protected fallback ancestors,
   gutters, and mixed-level drawing.
4. Add live CPU/GPU/GDAL metrics and settings enforcement.

Exit: pan/zoom tests show coarse-first coverage and eventual native level 0;
accounted caches never exceed their limits; stale completions cannot resurrect
removed layers; repeated camera movement does not grow queues or RSS.

### Gate 3 — Catalog qualification

1. Enable VRT/GTI inputs and driver-specific diagnostics without enumerating
   catalog members in application code.
2. Add fixed-size BigTIFF/VRT/GTI stress fixtures, missing-driver behavior,
   insufficient-overview warnings, and qualification report fields.
3. Exercise the packaged macOS, Windows, Linux, and Flatpak GDAL builds, not
   only the developer machine's driver set.

Exit: the large-source acceptance test meets the read-count and memory bounds,
the packaged app reports GTI available, and zooming reaches native source
pixels in a selected catalog tile.

## Pitfalls

The failure modes below are specific to this design and are the ones most
likely to surface late, when they are expensive. Each is addressed above; they
are collected here because they are easy to lose in a long specification.

- **Assuming overview index equals reduction.** An overview at array index 2
  need not be an 8x reduction, and bands need not expose identical arrays.
  Match common levels by dimensions and use measured ratios everywhere.
- **Hysteresis applied to the wrong level.** Test refinement against the
  current level but coarsening against the next coarser candidate. Comparing
  both thresholds with the current level can flap or create a dead zone when
  overview ratios are not 2x.
- **A coarse tile that is not cheap.** Requesting a level with no backing
  overview makes GDAL scan the source. This does not fail; it hangs, and it
  hangs worst on exactly the large catalogs the feature exists to serve.
- **Invisible memory.** GDAL's global block cache will satisfy a bounded-cache
  assertion while resident memory grows past any budget the application
  believes it is enforcing.
- **Treating a version floor as a driver probe.** GDAL 3.9 is necessary for
  the supported GTI baseline, but optional drivers still require runtime
  capability checks.
- **Halos at nodata edges.** Filtering color and mask separately is the
  default outcome of a straightforward implementation and is wrong; it shows
  up on every mosaic boundary.
- **Coplanar z-fighting.** Two pipelines computing "the same" depth at
  `z = 0` will disagree in the last bits. A single-frame test will not catch
  the resulting shimmer.
- **Full-range 16-bit.** Correct-looking code that renders every 16-bit
  orthophoto near-black.
- **Rejecting terrain.** Treating float single-band rasters as unsupported
  excludes the most common non-photographic raster in this application's
  domain.
- **Forcing statistics during import.** `GetStatistics(TRUE, TRUE)` is allowed
  to scan the dataset and can make a metadata job as expensive as loading the
  source. Use the non-forcing call and the bounded sampler only.
- **Mismatched band pyramids.** Reading RGB from one overview size and alpha
  from another misregisters transparency. Only expose the dimension
  intersection common to every required band.
- **Stale asynchronous results.** A tile decoded for an old range/ramp or a
  removed layer can otherwise be uploaded after cancellation. Validate layer,
  generation, and epoch on the render thread before cache admission.
- **Dataset-handle multiplication.** Each worker handle may cause a VRT/GTI to
  open many child datasets and driver-private caches. Default to two workers,
  measure RSS, and do not equate GDAL block-cache bytes with all GDAL memory.
- **Variant exhaustiveness.** Existing code often treats a non-point payload
  as vector. Every such site must be changed before adding `RasterLayerState`
  or it will throw `std::bad_variant_access`.
- **Resource destruction order.** QRhi textures, SRBs, and pipelines must be
  released while their owning QRhi is valid; workers and queued callbacks must
  be stopped before the viewport and source objects disappear.
- **Geographic wraparound.** A non-reprojected raster crossing the antimeridian
  cannot be represented correctly by a simple min/max XY bound. Detect it via
  `crossesAntimeridian`, reject the import, and cover it with a fixture. A bad
  bound here is not confined to the offending layer: it corrupts scene fitting
  and framing for the whole document.

### Risk register

| Risk | Severity | Early signal | Required mitigation |
|---|---:|---|---|
| GDAL reads base pixels for a coarse view | Critical | Read bytes/time grows with source dimensions | Read only explicit common overview bands; instrument requested level/window and source bytes. |
| Raster import blocks the UI | Critical | Event-loop stall during open/statistics | Run open, driver probing, and bounded sampling in `RasterLoadController`; never force statistics. |
| Unbounded memory outside tile LRUs | Critical | RSS rises while cache counters remain flat | Set/report GDAL cache, reserve in-flight scratch, cap keys/workers, and track process RSS. |
| Use-after-free during cancellation | Critical | Crash after removing/replacing a loading layer | Shared source lifetime, weak queued target, stop tokens, epoch/generation validation, deterministic shutdown test. |
| QRhi resource misuse | Critical | Backend validation errors/device loss | Create/upload/destroy only on render thread and outside active render passes; run validation-enabled GPU lane. |
| Missing native detail | High | Zoomed checkerboard never reaches source pattern | Level 0 is mandatory, projected-error tests clamp to it, and GPU acceptance reads back native detail. |
| LOD thrash/cache churn | High | Alternating level metrics and repeated reads | Candidate-level hysteresis, prior-selection state, center priority, and a bidirectional zoom sweep test. |
| Overview mismatch across bands | High | Colored/transparent edges are spatially offset | Dimension intersection and per-band overview indices; reject non-common levels. |
| Coplanar shimmer with points | High | Flicker changes with camera/backend | QRhi depth/slope bias, points rendered first, no raster depth write, multi-frame camera-sweep GPU test. |
| Seams between tiles or levels | High | One-pixel lines during pan/refinement | Replicated gutters, inner UV rect, exact outer edge, parent-first/child-second draw order. |
| Catalog handle explosion | High | File descriptors or RSS scale with worker count | Two workers by default, exclusive pooled handles, low configurable maximum, handle/RSS diagnostics. |
| Wrong geospatial placement | High | Raster mirrored, half-pixel shifted, or rotated incorrectly | Use pixel edges and full six-term affine; fixtures with rotation, negative pixel height, and known corner colors. |
| Stale style colorization | Medium | Old ramp flashes after style change | Render generation in CPU/GPU keys; opacity/Z excluded from decode generation. |
| Event-loop wake storm | Medium | UI CPU spike after many reads finish | Atomic wake coalescing and bounded completion drain per frame. |
| Excess draw/SRB overhead | Medium | CPU command time grows with visible tiles | 512 selected-tile cap, telemetry, persistent SRBs, dynamic uniforms, and later atlas/array optimization only if measured. |
| Unsupported catalog packaging | Medium | Works locally, fails in distributed app | Runtime capability report plus packaged smoke tests for GTI and the chosen index format. |

The first six risks are release blockers. Do not waive them based on a small
local GeoTIFF demonstration; their failure modes appear only with large,
remote, rotated, or rapidly changing inputs.

## Test Plan and Acceptance Criteria

- Unit-test affine corner/bounds calculations, rotated imagery, inverse
  transforms, LOD thresholds, frustum traversal on enormous logical rasters,
  parent fallback, request prioritization, cancellation, negative caching,
  and CPU/GPU eviction limits.
  - Include an explicit LOD stability test: sweep projected texel size
    continuously across each level boundary in both directions and assert the
    selected level changes monotonically and never oscillates between
    consecutive samples.
  - Include an explicit precedence test for pinned tiles versus budget,
    asserting that the planner coarsens before its protected set can exceed
    capacity and that admission never evicts a protected tile.
  - Include overview-intersection fixtures whose RGB and mask bands expose
    different overview arrays. Assert that only matching dimensions enter the
    level table and no request names an implicit/unbacked level.
  - Test `FrustumCuller::clipConvexPolygon()` on its own, before any raster
    code consumes it: a quad wholly inside, wholly outside, straddling each of
    the six planes in turn, and degenerating to a point or empty set.
  - Include a cross-frame hysteresis test using a non-power-of-two level table:
    hold the camera fixed and assert two consecutive plans are identical, then
    sweep slowly across a level boundary and assert no region changes level
    more than once. A key-equality state lookup passes the first and fails the
    second.
  - Test cache budget reduction: fill to capacity, lower the budget, and assert
    resident bytes fall to the new limit immediately rather than at the next
    admission. Cover both the CPU and GPU caches.
  - Assert the effective selected-tile cap is
    `min(maximumSelectedTiles, gpuCapacityTiles)` by driving each limit below
    the other in turn.
- Component-test GDAL inspection and tile reads using generated GeoTIFF
  fixtures covering RGB, RGBA, grayscale, palette, UInt16, Float32 terrain,
  masks/nodata, overviews, missing overviews, world-file georeferencing,
  rotated transforms, VRT mosaics, and GTI catalogs.
  - Include a nodata-boundary fixture and assert no color bleed across the
    boundary at a downsampled level, which a checkbox-level "masks/nodata"
    fixture would not catch.
  - Include an un-overviewed mid-size fixture (about 5000x5000) and assert it
    renders through the bounded base-band path, plus one above
    `rasterBoundedBaseReadPixels` that reports "tiled rendering required" and
    performs no base scan. Assert the read count for the second, not just its
    message.
  - Include a geographic fixture crossing the antimeridian and assert the
    import fails with that named reason rather than producing world-spanning
    bounds.
  - GTI capability is required in the main supported CI/package lane and its
    absence fails that lane. A specifically `.gti.gpkg` fixture may skip when
    optional GPKG support is absent, but the core GTI fixture must use an index
    format guaranteed by the lane.
- Test scene snapshots and UI behavior for multiple raster layers, mixed
  point/vector/raster ordering, visibility, isolation, removal, framing,
  default/edited Z, opacity, display range, warnings, partial import failure,
  cancellation, and retry.
- Add native GPU tests proving:
  - Known pixels land at their georeferenced XY coordinates on `z = 0`.
  - A high-frequency source first displays coarse coverage and resolves to
    its native pattern after zooming.
  - Multiple raster layers obey painter order and opacity.
  - Points at `z = 0` remain visible with EDL both enabled and disabled, held
    stable across a multi-frame camera sweep rather than asserted on one
    frame.
  - Raster textures stay within the configured GPU budget through repeated
    pan/zoom eviction.
- Add a large-source acceptance test using a sparse BigTIFF plus a GTI
  catalog of **fixed enormous logical size** — not one sized relative to
  available memory, which would behave differently on CI than on a
  workstation and give a non-deterministic result. Passing requires bounded
  CPU, GPU, and GDAL cache metrics; a total read count that is not
  proportional to the catalog's pixel count; visible coarse coverage before
  full detail; and eventual level-0 rendering when zoomed to native
  resolution.

Suggested verification order for each gate:

```sh
cmake --build --preset development --parallel
ctest --test-dir build/development -R 'raster|scene|architecture' --output-on-failure
ctest --preset development
```

Run the raster GPU target separately with the repository's configured native
API and validation enabled. The large GTI test is a labeled
`component;raster;stress` test with a fixed timeout and belongs in a scheduled
or qualification lane if it is too slow for every pull request; its smaller
capability fixture remains in the normal component lane.

## Assumptions, Non-Goals, and Limitations

- "Image collection" means an existing GDAL dataset representing a
  mosaic/catalog; the application will not construct collections from
  arbitrary selected files in this phase.
- Rasters are planar overlays only: no terrain draping, reprojection,
  editing, persistence in a project format, scientific band-selection UI, or
  generated overview/disk cache.
- Float terrain values are colorized on the flat plane; they do not displace
  vertices. ICC/profile-based color management and gamma-correct resampling
  are also outside this phase; decoded output is premultiplied RGBA8.
- Rasters are not lit by EDL, not pickable, and not measurable.
- Display quality on a dataset without adequate overviews is bounded by what
  the dataset provides. The application reports this rather than compensating
  for it; generating overviews remains the user's responsibility, via
  `gdaladdo` or an equivalent.
- Local files are the primary UI input, although references inside VRT/GTI
  datasets may use any virtual filesystem supported by the installed GDAL
  build.

## Technical References

- [GDAL raster data model and overview bands](https://gdal.org/en/stable/user/raster_data_model.html)
- [GDALRasterBand API, including non-forcing statistics](https://gdal.org/en/stable/doxygen/classGDALRasterBand.html)
- [GDAL runtime configuration and block-cache behavior](https://gdal.org/en/stable/user/configoptions.html)
- [GDAL GTI raster tile-index driver](https://gdal.org/en/stable/drivers/raster/gti.html)
- [GDAL VRT raster driver and overview behavior](https://gdal.org/en/stable/drivers/raster/vrt.html)
- [Qt QRhi graphics-pipeline depth state and bias](https://doc.qt.io/qt-6/qrhigraphicspipeline.html)

## Recommendations

- **Ship phase 1 before writing any streaming code.** The scene-model and
  renderer edits are the widest and least reversible part of this work, and
  they are independent of tiling. Landing them alone keeps the eventual
  streaming review focused on streaming.
- **Treat overview availability as a first-class input, not an optimization.**
  It determines the usable level table, layer warnings, and whether the
  large-source acceptance test can pass at all. Intersect levels at open time
  and never ask GDAL to invent a runtime level from the base image.
- **Budget all three allocators or none.** Partial accounting is worse than
  none, because it produces confident metrics that are wrong.
- **Use runtime driver probes in addition to the GDAL 3.9 floor,** and keep
  the qualification report specific enough that a user's environment can be
  diagnosed from it.
- **Support float terrain rasters in phase 1's format matrix.** Include the
  default range/ramp metadata immediately; editing controls can follow, but a
  layer contract that rejects float sources would have to be reworked rather
  than extended.
- **Defer palette raster support if scope pressure appears.** Indexed imagery
  is comparatively rare in this domain, and its nearest-neighbor path is the
  most isolated piece to add later.
