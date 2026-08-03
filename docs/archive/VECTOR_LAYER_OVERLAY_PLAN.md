# Planar 2D vector overlays via GDAL — implementation plan

Status: archived 2026-07-31; this 2026-07-30 vector-overlay implementation plan is superseded by the implemented system and the Point Cloud Inspector Modernization Refactoring Guide.

Status: implementation in progress. The architecture and product decisions are
decision-complete; §7 tracks the remaining implementation and qualification work.
Target: `pcinspector` 0.1.x · Qt 6.7+ / QRhi · PDAL 2.10 · GDAL 3.4+.

This document is intended to be executable by a developer who has not been part of
the design discussion. Every claim marked **verified** was checked against this
codebase or this toolchain; §3 lists the evidence.

---

## 1. The problem

`pcinspector` renders point clouds and nothing else. A user inspecting a LAS tile
has no reference geometry — no parcel boundaries, no building footprints, no road
centrelines, no survey control points — so a scan is hard to orient, hard to explain
to somebody else, and hard to check against known ground truth.

This plan adds **vector layers**: an OGR-readable local vector file supported by the
compiled GDAL drivers is loaded and drawn into the 3D scene as filled polygons,
lines, and point markers, listed in the existing Scene panel beside point-cloud
layers and styled per layer.

### What this is, precisely

**A planar vector overlay, not terrain draping.** Each layer is rendered on one
constant-Z plane. It does not follow terrain, and per-vertex elevation sampling is
out of scope (§11). Getting this distinction into the UI and the docs matters,
because "vector layers over a point cloud" invites the draping interpretation and
the product does not deliver it.

### Why this is not simply "draw some lines"

Five properties of the existing codebase shape the whole design:

1. **World coordinates are large and float32 is not enough.** `test_data/3445-343.laz`
   spans northing 7 400 188–7 400 775. At 7.4 × 10⁶ a float32 ULP is **0.5 m**.
   Anything naïvely uploaded as float32 world coordinates collapses visibly. The
   renderer already solves this with camera-relative rendering and per-block
   quantisation (`RenderViewportWidget.cpp:2121-2128`, `src/core/GpuPoint.h`); vector
   layers must adopt the same discipline (§5).
2. **Render passes clear.** `PointCloudRenderer::draw` owns its `beginPass`/`endPass`
   and clears (`PointCloudRenderer.cpp:181-199`), and so does
   `EyeDomeLightingPass::composite` (`EyeDomeLightingPass.cpp:156-168`). QRhi has no
   load-preserve for these. A fourth appended pass would erase the point image, so
   pass ownership has to move (§6.7).
3. **Wide lines are not portable.** Metal has no line width. Stroke width must be
   screen-space quad expansion in a vertex shader (§6.8).
4. **The frame is event-driven and budgeted.** Idle frames cost zero. An adaptive
   controller reacts to *total* frame time, so vector cost is not free (§1.1).
5. **Untrusted input.** Real SHP/DXF/GML data contains null geometries,
   self-intersecting cadastral rings, single features with millions of vertices, and
   curves that linearise without bound. Limits and a validation contract are load
   bearing, not defensive polish (§6.3).

### 1.1 Performance contract

The earlier draft claimed vector work would be "invisible" to the adaptive point
budget. **That was false.** `AdaptivePointBudget::update` consumes
`FrameSample::frameTime` (`src/core/AdaptivePointBudget.h:9`), which is the whole
frame measured by `frameDuration(commandBuffer)` — GPU time for the entire command
buffer, or CPU wall time. Expensive vector drawing therefore *will* reduce point
detail. That is arguably correct behaviour, but it is not invisibility.

The honest contract, which §9 turns into measurable criteria:

> **Zero draw cost when no vector layer is present or all are hidden. Bounded,
> measured overhead for admitted visible layers. Point detail may yield to a visible
> vector layer exactly as it yields to any other frame cost.**

### Goals

- Load points, lines, and **filled** polygons (with holes) from an OGR-readable local
  vector file supported by the compiled drivers.
- One style per layer: fill, stroke, marker, opacity, Z offset, always-on-top.
- Hard, enforced limits on application-owned input, emitted geometry, retained
  payload, and working storage, with the one-feature GDAL allocation boundary stated
  explicitly.
- Loading off the GUI thread, cancellable mid-feature, visible in the Tasks dock.

### Non-goals

Listed in full in §11. The significant ones: terrain draping, CRS reprojection,
attribute-driven styling, labels, and feature picking.

---

## 2. Decisions and their rationale

| Decision | Rationale |
|---|---|
| Points, lines, **filled** polygons | Footprints and parcels are the primary use case; outlines alone are not enough. |
| **One style per layer** | Attribute-driven styling triples the UI and uniform plumbing for a first cut. |
| **No CRS reprojection**, but CRS *protection* | The test data carries no SRS at all, which is typical, so a reprojection path would mostly have nothing to reproject. "No reprojection" is a reasonable boundary; "no mismatch warning" is not (§6.4). |
| Depth-tested + per-layer **always on top** | A ground-plane layer must sit correctly under terrain, but a layer hidden by dense points is useless; the toggle covers both. |
| Vendor **mapbox/earcut.hpp** | Single ISC header, handles holes, 10–50× faster than GEOS constrained Delaunay. |
| **Sublayer picker dialog** | A GPKG or GML can hold dozens of sublayers; dumping them all into the Scene panel is hostile, and loading only the first is silently wrong. |
| Vector layers in the **same document**, sharing one id space | Removes roughly a dozen parallel UI and renderer methods (§6.6). |
| **Fail, do not truncate**, on limit violation | A partially clipped polygon layer is untrustworthy and a silent truncation reads as success (§6.3). |

---

## 3. Verified environment findings

Everything here was checked, not assumed. Where a claim is *unverified* it says so.

### GDAL

- **GDAL 3.13.1** at `/opt/homebrew` exports `add_library(GDAL::GDAL SHARED IMPORTED)`
  (`lib/cmake/gdal/GDAL-targets.cmake:59`). `libpdalcpp` already links
  `libgdal.39.dylib`, so it is loaded into the process today — but Homebrew's
  `PDALConfig.cmake` only calls `find_package(GDAL)` for *static* PDAL, so
  `GDAL::GDAL` is **not** defined in our scope. We must find it ourselves.
- **CMake's own `FindGDAL` module also defines `GDAL::GDAL`**
  (`FindGDAL.cmake:240`, shipped with CMake 4.2.1). So `find_package(GDAL 3.4 REQUIRED)`
  **without** `CONFIG` is strictly more portable — it uses a config package when one
  exists and falls back to the module on distributions that ship none.
- The OGR C++ API **builds and works** with this toolchain: `clang++ -std=c++23
  -I/opt/homebrew/include -lgdal` opened a GeoJSON polygon with a hole and reported
  `layers=1`, `type=Polygon`, `features=1`,
  `extent=334100.00 7400200.00 334500.00 7400700.00`, `rings=2 exterior_pts=5
  hole_pts=5 curve=0`. Two findings fell out:
  - **`OGRLayer::GetExtent()` is `warn_unused_result`.** Ignoring the return compiles
    normally but **fails the `ci` preset** (`PCINSPECTOR_WARNINGS_AS_ERRORS=ON`).
  - **A CRS-less GeoJSON still reports a spatial reference** — the driver supplies the
    format's default OGC:CRS84. So GDAL can confidently report WGS84 for a file whose
    coordinates are plainly projected. The Inspector label must therefore read
    **"CRS reported by GDAL"**, not "declared".
  - `exterior_pts=5` for a four-corner square confirms **OGR rings are closed**, which
    drives the earcut de-duplication in §6.3.
- Multi-sublayer GPKG fixtures are viable: writing two named layers into one `.gpkg`
  and enumerating them returns `1: parcels (Polygon)` / `2: buildings (Polygon)`.
- **Version floor is unverified.** Every OGR API this plan uses is ≥ 3.0
  (`GDALDatasetUniquePtr` 3.1, `SetAxisMappingStrategy` 3.0, `OGRFeatureUniquePtr` 2.3,
  `hasCurveGeometry` 2.0), and **3.4** is declared because Ubuntu 22.04 LTS ships
  3.4.1 — but 3.13.1 is the only build tested. A CI lane at the declared minimum is a
  **prerequisite for advertising 3.4 support** (Phase 9), not a follow-up.

### Shaders and QRhi

- **`gl_FragDepth` survives Qt's shader pipeline.** `qsb --glsl 100es,120,150
  --hlsl 50 --msl 12` succeeds and emits `float gl_FragDepth [[depth(any)]]` in MSL,
  `SV_Depth` in HLSL, and a `FragDepth` entry under `outBuiltins`.
- **`noperspective` also survives, and lands where it must** — `[[user(locn0)]]` on
  the MSL vertex output and `[[user(locn0), center_no_perspective]]` on the fragment
  input. Kept as a documented fallback; §6.8 uses a `w = 1` pre-divide instead.
- **`clip.w` is exactly view-space depth, and the eye-plane test is not a near-plane
  test.** A `QMatrix4x4` harness with `near = 2.0` and a Vulkan-style correction
  matrix gave:

  | view depth | `clip.w` | `w == depth` | `ndc.x` |
  |---|---|---|---|
  | −5.00 | −5.000 | yes | −0.693 |
  | **0.50** | **0.500** | yes | **6.928** |
  | 1.00 | 1.000 | yes | 3.464 |
  | 2.00 | 2.000 | yes | 1.732 |
  | 100.00 | 100.000 | yes | 0.035 |

  A point at depth 0.5 with `near = 2.0` passes a `w > 1e-4` test and then projects to
  `ndc.x = 6.93` — roughly seven times outside the viewport, producing an enormous
  quad and a direction vector dominated by the blown-up endpoint. **This is a real
  bug**, and because the correction matrix leaves `w` untouched, testing
  `w < nearPlaneDistance` is the correct backend-independent fix (§6.8).
- `PreserveColorContents` / `PreserveDepthStencilContents` (`qrhi.h:1226-1227`);
  `setDepthTest` / `setDepthWrite` / `setDepthOp` (`:1451-1460`); `WideLines` is
  **optional** (`:1904`); `QRhiVertexInputBinding::PerInstance` (`:184`);
  `TriangleStrip` (`:1322`). `TargetBlend` defaults to premultiplied-over
  (`srcColor = One`, `dstColor = OneMinusSrcAlpha`). `QRhiWidget` allocates a
  `depthStencilBuffer` (`qrhiwidget_p.h:58`). **`sampleCount` is never set anywhere**,
  so the target is 1× — there is no MSAA to lean on.
- **depthWrite without depthTest is backend-divergent.** Vulkan gates depth writes on
  `depthTestEnable`, so they silently vanish; QRhi's Metal backend sets
  `depthCompareFunction` and `depthWriteEnabled` independently, so they land. The
  portable form is `depthTest(true)` + `depthOp(Always)` + `depthWrite(true)`.

### Existing code that constrains the design

| Fact | Location |
|---|---|
| Adaptive budget consumes whole-frame time | `src/core/AdaptivePointBudget.h:9` |
| The document builds its **own** hierarchy scheduler — separate from the import one | `src/scene/PointCloudDocument.cpp:48-54` |
| `~PointCloudLoadController` calls `scheduler_->waitForIdle()` | `src/import/PointCloudLoadController.cpp:160` |
| Scheduler has a real fairness mechanism (`fairnessGroup`, `groupActive_`) | `src/scene/PointTaskScheduler.h:55-95` |
| Tasks dock stores only `PointCloudLoadJobState`, resolves actions by bare `jobId` | `src/app/PointCloudLayerPanel.cpp:768`, `:993-996` |
| `requestLoadCancellation()` early-returns unless `loading_` | `src/app/MainWindow.cpp:999-1002` |
| `isolateLayer` / `showAllLayers` iterate **point layers only** | `src/app/MainWindow.cpp:1175-1187`, `:1189+` |
| Replace retains `load.previousDocument` for rollback | `src/app/MainWindow.cpp:1553` |
| `setDocument` resets `seenDocumentRevision_ = 0` and clears every cache | `src/renderer/rhi/RenderViewportWidget.cpp:334` |
| `bounds()` feeds point colour normalisation, not just framing | `src/scene/PointCloudDocument.h:76-78` |
| `empty()` is the replace-or-add predicate at ten call sites | `src/app/MainWindow.cpp:795`, `:816`, `:818`, `:948`, `:977`, … |
| `AUTOMOC` is globally **OFF**; `app_ui` and `import_async` opt in | `CMakeLists.txt:80`; `src/CMakeLists.txt:156`, `:188` |
| Installed builds are a supported artifact | `CMakeLists.txt:108-119` |
| Item data roles `Qt::UserRole + 0/1/2` are taken | `src/app/PointCloudLayerPanel.cpp:43-45` |

---

## 4. Architecture

```
                     third_party/earcut  (INTERFACE, SYSTEM include)
                              |
core ── pointcloud ──── pcinspector_vector ─────┐
                              |                  |
                              |            import_ogr ── GDAL::GDAL (PRIVATE)
                              |                  |
                            scene ───────────────┘
                              |
              ┌───────────────┼────────────────┐
        import_api      renderer_api      import_pdal ── PDAL::pdalcpp (PRIVATE)
              |               |                  |
         app_model      renderer          import_async  (+ VectorLoadController)
              |               |                  |
              └────────── app_ui ────────────────┘
                              |
                        pcinspector
```

`pcinspector_vector` is pure C++ — no Qt, no GDAL — so all geometry maths, limits,
and validation are unit-testable without a device or a dataset.
`pcinspector_import_ogr` links `GDAL::GDAL` **PRIVATE**, mirroring how
`pcinspector_import_pdal` hides PDAL (`src/CMakeLists.txt:135-140`), so OGR headers
never reach `app_ui`.

### New files

| Path | Target | Role |
|---|---|---|
| `third_party/earcut/include/mapbox/earcut.hpp`, `LICENSE`, `README.md` | `pcinspector_earcut` | Vendored ISC triangulator, unmodified, pinned upstream SHA |
| `src/vector/VectorGeometry.h/.cpp` | `pcinspector_vector` | Builder: split limits, ring→segment expansion, earcut fills, triangle-stream fill partitioning, area validation |
| `src/vector/VectorLayerStyle.h/.cpp` | `pcinspector_vector` | Style value struct, defaults, clamping |
| `src/vector/VectorLayerData.h/.cpp` | `pcinspector_vector` | Immutable compiled layer payload |
| `src/vector/VectorImport.h` | `pcinspector_vector` | Loader contract, request/preflight/limits/errors, origin probe, load summary (no GDAL/Qt) |
| `src/import/ogr/OgrRuntime.h/.cpp` | `pcinspector_import_ogr` | Driver registration, scoped CPL error capture |
| `src/import/ogr/OgrVectorLoader.h/.cpp` | `pcinspector_import_ogr` | `inspect()` / `loadSublayer()` |
| `src/import/VectorLoadController.h/.cpp` | `pcinspector_import_async` | Single-job orchestration on the shared import scheduler |
| `src/import/LoadJobKey.h` | `pcinspector_import_api` | Pure C++ `LoadJobKind`, `LoadJobKey`, `LoadJobCapabilities` |
| `src/import/LoadJobRow.h/.cpp` | `pcinspector_import_async` | `QString`-based Tasks-row projection shared by both controllers |
| `src/import/ImportServices.h/.cpp` | `pcinspector_import_async` | Shared scheduler plus point-cloud and vector controllers; production/test factories |
| `src/renderer/rhi/VectorLayerRenderer.h/.cpp` | `pcinspector_renderer` | Pipelines, GPU buffers, draw recording |
| `shaders/vector_fill.{vert,frag}`, `vector_line.{vert,frag}`, `vector_marker.{vert,frag}` | shaders | Three programs |
| `src/app/VectorSublayerDialog.h/.cpp` | `pcinspector_app_ui` | Sublayer picker |
| `src/app/VectorColorButton.h/.cpp` | `pcinspector_app_ui` | RGBA swatch button |

### Modified files

`CMakeLists.txt` (find GDAL) · `src/CMakeLists.txt` (targets) ·
`cmake/PciShaders.cmake` (shader inventory) · `src/scene/PointCloudDocument.*`
(→ `SceneDocument`, vector collection, `isolateLayer`, `setAllLayersVisible`) ·
`src/renderer/RenderViewport.h` · `src/renderer/rhi/RenderViewportWidget.*` ·
`src/renderer/rhi/PointCloudRenderer.*` · `src/renderer/rhi/EyeDomeLightingPass.*` ·
`shaders/edl.frag` · `src/import/PointCloudLoadController.*` (scheduler ownership) ·
`src/app/MainWindow.*` · `src/app/PointCloudLayerPanel.*` ·
`src/renderer/RenderMetrics.h` · `tests/CMakeLists.txt`.

---

## 5. Coordinates and precision

This is the part that silently ruins the feature if it is got wrong.

| Coordinate magnitude | float32 ULP |
|---|---|
| 6.5 × 10⁶ — raw SWEREF99 TM northing | **0.5 m** |
| 2 × 10⁵ — 200 km from origin | 16 mm |
| 1 × 10⁴ — 10 km | 0.98 mm |
| 1 × 10³ — 1 km | 61 µm |

Every layer carries a `Vec3d origin` in double and stores all vertices as float32
**relative to it**. The origin is snapped to a 1024 m grid so it is reproducible
across runs and shared between sublayers of one file:

```cpp
[[nodiscard]] Vec3d vectorLayerOrigin(double x, double y) noexcept
{
    return {std::floor(x / 1024.0) * 1024.0,
            std::floor(y / 1024.0) * 1024.0,
            0.0};
}
```

**Origin selection, specified fully** (an earlier draft was contradictory — it
promised a snapped shared origin while accepting a `Vec3d` cloud centre whose Z had
nowhere to go):

1. `VectorImportRequest::origin` is `std::optional<std::array<double, 2>>` — **X/Y
   only**, so the Z contradiction cannot arise. Layer origin Z is always 0.
2. When the UI supplies a value (the point-cloud bounds centre), the **loader** snaps
   it to the 1024 m grid. Callers never pre-snap.
3. With no point cloud, the controller supplies the centre of the **union of all
   selected sublayer extents** and the loader snaps it, so every selected sublayer of
   one dataset shares one origin.
4. If any selected sublayer has no reported extent, the controller submits exactly
   one `OriginProbe` task before any per-sublayer load task. It visits selected
   sublayers in preflight order and features/geometry children in source order,
   polling cancellation with the same traversal cadence as loading, and returns the
   first finite X/Y coordinate. The loader snaps that coordinate and every selected
   sublayer uses the result. This makes an extent-less shared origin deterministic
   rather than whichever concurrent sublayer happens to produce a coordinate first.
   An empty probe fails the logical job before any sublayer is emitted.
5. `maximumOriginRelativeMetres = 200'000`. A layer whose extent exceeds ± 200 km from
   its origin **fails** with a message naming the extent, replacing the earlier
   informal "about 1000 km". At 200 km the ULP is 16 mm — below one screen pixel at
   any practical zoom.

The origin is folded into the model matrix **on the CPU, in double**, exactly as
point blocks already do — never added back in a shader:

```cpp
// mirrors RenderViewportWidget.cpp:2121-2128
const Vec3d relative{origin.x - eye.x, origin.y - eye.y, zOffset - eye.z};
QMatrix4x4 model;
model.translate(static_cast<float>(relative.x),
                static_cast<float>(relative.y),
                static_cast<float>(relative.z));
const QMatrix4x4 mvp = viewProjection * model;
```

Subtracting the origin before triangulation also helps earcut, whose area and
intersection arithmetic then works on ~10⁴-magnitude doubles instead of ~6.5 × 10⁶.

---

## 6. Design

### 6.1 Style

`src/vector/VectorLayerStyle.h` — non-premultiplied sRGB, channels in [0, 1].
Defaults come from the Strata accent `#4c8dff` (`src/app/StrataTheme.cpp`). It
includes `vector/VectorGeometry.h` for `VectorGeometryKind`; the dependency is
one-way, since `VectorGeometry.h` knows nothing about styling.

```cpp
struct VectorRgba {
    float red = 0.0F, green = 0.0F, blue = 0.0F, alpha = 1.0F;
    bool operator==(const VectorRgba &) const = default;
};

enum class VectorMarkerShape : std::int32_t { Circle = 0, Square = 1 };
static_assert(static_cast<int>(VectorMarkerShape::Circle) == 0);   // shader ABI
static_assert(static_cast<int>(VectorMarkerShape::Square) == 1);

inline constexpr float maximumVectorStrokeWidthPixels = 20.0F;
inline constexpr float maximumVectorMarkerSizePixels = 64.0F;
inline constexpr double maximumVectorZOffsetMagnitude = 1.0e6;

struct VectorLayerStyle {
    VectorRgba fill{0.298F, 0.553F, 1.0F, 0.35F};
    VectorRgba stroke{0.298F, 0.553F, 1.0F, 1.0F};
    VectorRgba marker{1.0F, 0.72F, 0.20F, 1.0F};
    float strokeWidthPixels = 1.5F;
    float markerSizePixels = 6.0F;
    VectorMarkerShape markerShape = VectorMarkerShape::Circle;
    float opacity = 1.0F;        // multiplies every alpha at draw time
    double zOffset = 0.0;        // metres added to the layer plane
    bool alwaysOnTop = false;    // draws with the depth compare disabled
    bool operator==(const VectorLayerStyle &) const = default;
};

[[nodiscard]] VectorLayerStyle defaultVectorLayerStyle(VectorGeometryKind) noexcept;
// Clamps every numeric field and replaces non-finite values with the default, so no
// UI path can install an unusable style. Applied by the document on every set.
[[nodiscard]] VectorLayerStyle clampVectorLayerStyle(VectorLayerStyle) noexcept;
```

Line layers default to a zero-alpha fill, polygon layers to a 35 % fill, point layers
to a 7 px circle. Triangle and cross markers are a deliberate follow-up; the enum
leaves room. Every `*Pixels` value is measured in **device/render-target pixels**,
matching the existing point-size contract. `RenderViewportWidget` passes the render
target's pixel dimensions and applies **no `devicePixelRatio` scaling** to vector
stroke, marker, or feather widths.

### 6.2 Compiled layer payload

```cpp
// src/vector/VectorGeometry.h
struct VectorVertex2f  { float x = 0.0F, y = 0.0F; bool operator==(const VectorVertex2f &) const = default; };
struct VectorSegment2f { float x0 = 0.0F, y0 = 0.0F, x1 = 0.0F, y1 = 0.0F; bool operator==(const VectorSegment2f &) const = default; };

enum class VectorGeometryKind : std::uint8_t { Point = 0, Line = 1, Polygon = 2 };

// One fill batch never exceeds maximumVerticesPerFillBatch, so 16-bit indices
// always suffice and QRhi::ElementIndexUint is never required. See 6.8.
inline constexpr std::size_t maximumVerticesPerFillBatch = 65'536;

struct VectorFillBatch {
    std::vector<VectorVertex2f> vertices;   // <= maximumVerticesPerFillBatch
    std::vector<std::uint16_t> indices;
};
```

**Counters distinguish source features from geometry parts.** An earlier draft named
these `pointFeatures` / `lineFeatures` / `polygonFeatures` while incrementing them
inside collection recursion, so one `GeometryCollection` inflated the "feature" count
— and an unsupported *child* was counted as a skipped *feature*, which it is not.

```cpp
struct VectorGeometrySummary {
    // Geometry parts, incremented inside recursion.
    std::uint64_t pointParts = 0, lineParts = 0, polygonParts = 0;
    // Top-level OGR features with no usable geometry at all.
    std::uint64_t skippedFeatures = 0;
    // Unsupported or degenerate children inside a collection or multi-geometry.
    std::uint64_t skippedParts = 0;
    // Rings that failed the validation contract in 6.3. Outline is still drawn.
    std::uint64_t unfilledPolygons = 0;
    bool sourceHadZ = false;

    [[nodiscard]] VectorGeometryKind dominantKind() const noexcept;
};
```

There is no `truncated` flag: limit violations fail the sublayer (§6.3).

```cpp
// src/vector/VectorLayerData.h
struct VectorLayerData {                       // immutable once built
    Vec3d origin;                              // z always 0
    std::vector<VectorFillBatch> fillBatches;
    std::vector<VectorSegment2f> segments;      // strokes and polygon outlines
    std::vector<VectorVertex2f> markers;
    Bounds3d bounds;                            // world space, z == [0, 0]
    VectorGeometrySummary summary;
    std::uint64_t featureCount = 0;             // consumed top-level OGR features
    std::filesystem::path sourcePath;
    std::string sourceDriver, sublayerName, spatialReferenceWkt;
    bool crsMismatch = false;                   // set when both CRSs known and differ
    bool extentDisjointXY = false;              // X/Y extents do not intersect

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint64_t byteSize() const noexcept;
    [[nodiscard]] std::uint64_t triangleCount() const noexcept;
    [[nodiscard]] std::string geometryDescription() const;   // Inspector text
};
using VectorLayerDataPtr = std::shared_ptr<const VectorLayerData>;
```

Storing **only X/Y** (Z comes from the model matrix) halves vertex memory and makes
the planar semantics explicit. Published as `shared_ptr<const>`, exactly like
`PointCloudScenePtr`, so document snapshots are cheap and safe to read from the
render thread.

### 6.3 Geometry compilation — limits, then validation

#### Limits

`maximumSourceFeatures` alone does **not** cap memory: one OGR feature can hold
millions of vertices, and `getLinearGeometry()` expands a `CIRCULARSTRING` by an
arbitrary factor.

```cpp
struct VectorImportLimits {
    std::uint64_t maximumSourceFeatures          = 2'000'000;
    std::uint64_t maximumInputPolygonVertices    = 16'000'000;
    std::uint64_t maximumEmittedFillVertices     = 16'000'000;
    std::uint64_t maximumFillIndices             = 48'000'000;
    std::uint64_t maximumSegments                =  8'000'000;
    std::uint64_t maximumMarkers                 =  8'000'000;
    std::uint64_t maximumRetainedBytes            = 256ULL * 1024 * 1024;
    std::uint64_t maximumApplicationWorkingBytes  = 256ULL * 1024 * 1024;
    std::uint64_t maximumCurveControlPoints       = 100'000;
    std::uint64_t maximumLinearizedCurvePoints    = 1'000'000;
    double curveMaximumAngleStepDegrees           = 4.0;
};
```

Enforced **incrementally inside `VectorGeometryBuilder`**, with overflow-safe
arithmetic checked *before* every reserve or append — never by inspecting totals
afterwards. Exceeding any limit throws `VectorImportLimitExceeded` (a
`VectorImportError`) naming the limit and the value reached, which **fails that
sublayer**. Other selected sublayers continue; the Tasks row reports which failed and
why.

The counters intentionally separate four quantities that are easy to conflate:

- `maximumInputPolygonVertices` counts ring coordinates presented to `addPolygon`
  before duplicate removal, including OGR's closing coordinate. It limits
  triangulation input, not the retained payload. Like every count limit here, it is
  cumulative across the sublayer.
- `maximumEmittedFillVertices` counts every vertex appended to a
  `VectorFillBatch`. A source vertex duplicated across a batch boundary counts again.
- `maximumFillIndices` counts the emitted triangle index stream.
- `maximumRetainedBytes` counts logical bytes retained by `VectorLayerData`:
  fill vertices and indices (including boundary duplicates), segments, markers,
  batch records, the `VectorLayerData` record, and copied metadata strings.
  `byteSize()` uses the same checked accounting.

`maximumApplicationWorkingBytes` is separate from retained bytes. It covers the
builder's retained payload plus application-owned transient rings, earcut input and
output, validation scratch, and original-to-local remap storage. Each application
allocation is preceded by a conservative checked charge and released from the
working counter when its scratch lifetime ends. Standard-library allocator capacity
and implementation overhead are not byte-exact, so the ceiling is an admission and
logical-allocation bound, not a process-RSS guarantee. "Application-owned" explicitly
excludes the current GDAL-owned feature geometry and any GDAL-owned linearised clone.
`curveMaximumAngleStepDegrees` must be finite and in `(0, 90]`; an invalid configured
value rejects the request before reading.

There is one unavoidable boundary: **GDAL materializes the current `OGRFeature` and
its geometry before application code can inspect its point counts, so GDAL's
allocation for one feature cannot be strictly capped.** Immediately after
`GetNextFeature`, traversal pre-checks coordinate/control-point counts before copying
or linearising anything; the application never compounds an already oversized GDAL
feature with unbounded builder allocation.

Truncation was considered and rejected: a partially clipped polygon layer cannot be
trusted for measurement or comparison, and a silent truncation reads as success.

#### Cancellation granularity

Polling `stopToken` every 4096 OGR features leaves a single huge feature effectively
uncancellable. Poll **inside traversal**: every 64 Ki appended coordinates, and at
every collection recursion boundary.

#### Builder interface

```cpp
using VectorRing = std::vector<std::array<double, 2>>;   // earcut-consumable

class VectorGeometryBuilder {
public:
    VectorGeometryBuilder(Vec3d origin, VectorImportLimits limits) noexcept;
    bool addPoint(double x, double y);
    bool addLineString(std::span<const std::array<double, 2>> vertices, bool closed);
    bool addPolygon(std::span<const VectorRing> rings);   // rings[0] exterior
    [[nodiscard]] VectorLayerData build(/* metadata */) &&;
};
```

Each `add*` returns false when the input was rejected as degenerate (and bumps a
counter); it *throws* only on a limit violation.

#### `addPolygon`, step by step

1. **Count, then reject early.** Overflow-sum every supplied ring size, enforce
   `maximumInputPolygonVertices`, and charge the normalization scratch before copying
   a coordinate. Empty `rings`, `rings[0]` with fewer than 3 distinct points, or any
   non-finite coordinate → `++skippedParts`, release scratch, return false. Non-finite
   must be caught *before* narrowing, because `static_cast<float>(NaN)` is a silent
   NaN that poisons an entire draw.

2. **Convert to origin-relative doubles, strip duplicate consecutive coordinates, and
   strip OGR's closing duplicate.**

   ```cpp
   // Consecutive duplicates create zero-area ears and degenerate segments.
   relative.erase(std::unique(relative.begin(), relative.end()), relative.end());
   // OGR rings are always closed; earcut expects OPEN rings.
   if (relative.size() >= 2 && relative.front() == relative.back()) {
       relative.pop_back();
   }
   ```

   The closing-duplicate strip is essential — verified in §3 that OGR reports
   `exterior_pts=5` for a four-corner square — but it is **not** the highest
   real-world risk. See step 4.

3. **Triangulate.** `mapbox::earcut<std::uint32_t>(relativeRings)`. earcut's default
   accessor uses `std::get<I>`, which works on `std::array` with no specialisation.
   Winding is irrelevant — earcut derives orientation from the exterior ring's signed
   area, so both shapefile (CW) and GeoJSON (CCW) conventions work. Returned indices
   address the **concatenation of all rings in order**, so step 5 must append them in
   that same order.

4. **Validate the result.** This is the real defence. Invalid and self-intersecting
   cadastral polygons are common, and **earcut returns plausible-but-wrong triangles
   rather than an empty result**, so an empty-result check catches nothing:

   - every index is in range;
   - every triangle area is finite;
   - the summed triangle area matches the shoelace area of the exterior ring minus
     holes, within 1 % of the ring area (or 1e-6 absolute for tiny rings).

   On failure: **fall back to outline-only**, `++unfilledPolygons`, and continue. No
   GEOS repair in v1 — this gives deterministic failure behaviour without the cost of
   validating every polygon through GEOS.

5. **Partition the triangle stream, not the polygon.** Iterate earcut's result three
   indices at a time. A dense `originalToLocal` table, indexed by the polygon's
   concatenated-ring index, stores a generation plus its `std::uint16_t` local index;
   the generation changes at each batch/polygon boundary, so clearing is O(1). Its
   `8 * normalizedPolygonVertexCount` bytes are charged to application working
   storage before allocation:

   - count how many of the triangle's distinct original indices are absent;
   - start a new batch **only if** appending those missing vertices would require
     more than 65,536 local vertices;
   - advance the remap generation for the new batch, append any triangle vertices not
     yet local, then append its three local indices;
   - advance the generation at a polygon boundary while retaining any remaining
     capacity in the batch, because original indices are polygon-local.

   A triangle is never split. A source vertex referenced on both sides of a batch
   boundary is deliberately duplicated; every such copy is charged to
   `maximumEmittedFillVertices`, `maximumRetainedBytes`, and the application working
   counter before append. Thus even one connected polygon with more than 65,536
   referenced vertices remains lossless while every batch keeps the universal
   16-bit-index policy.

6. **Stroke every ring, holes included** (`closed = true`).

7. `++polygonParts`; extend the world-space double bounds.

#### Ring → segment expansion

Shared by outlines and line strings:

- Same origin-relative conversion; strip consecutive duplicates; drop the trailing
  duplicate for closed rings.
- `n - 1` segments between consecutive vertices, plus a closing segment
  `(last, first)` when `closed`. An OGR-closed ring of *n* stored points therefore
  yields `n - 1` segments, the closing one included.
- **`wkbLinearRing` passes `closed = true`.** The earlier draft routed it to the open
  line-string path, which drops the closing segment — a straightforward bug.
- **Drop zero-length segments.** A degenerate quad has no direction vector and the
  screen-space expansion turns it into NaN.
- Fewer than two distinct vertices → nothing emitted, counted as skipped.
- No joint geometry is emitted: the fragment shader's capsule SDF (§6.8) gives round
  joins and caps for free.

### 6.4 GDAL / OGR import

`src/vector/VectorImport.h` holds the contract with **no GDAL header**, mirroring
`src/import/PointCloudImport.h:112-138`.

```cpp
struct VectorImportProgress {
    std::uint64_t processed = 0;   // top-level features read
    std::uint64_t total = 0;       // 0 when the driver cannot count cheaply
};

// Dataset enumeration index is the identity; name is display only. This handles
// unnamed and duplicate-named layers, and preserves request order.
struct VectorSublayerKey {
    int index = -1;
    std::string name;
    bool operator==(const VectorSublayerKey &) const = default;
};

struct VectorSublayerInfo {
    VectorSublayerKey key;
    std::string geometryTypeLabel, spatialReferenceWkt;
    std::optional<VectorGeometryKind> kind;   // nullopt for wkbUnknown (GeoJSON, DXF)
    std::int64_t featureCount = -1;           // -1 = unknown; never forced
    bool hasZ = false;
    std::optional<Bounds3d> extent;
};

struct VectorImportPreflight {
    std::filesystem::path sourcePath;
    std::string driverName;
    std::vector<VectorSublayerInfo> sublayers;
};

struct VectorImportRequest {
    std::filesystem::path sourcePath;
    std::vector<VectorSublayerKey> sublayers;          // empty = all
    std::optional<std::array<double, 2>> origin;       // X/Y only; loader snaps it
    VectorImportLimits limits;
    // Point-cloud CRS, for mismatch detection only. Never used to reproject.
    std::string targetSpatialReferenceWkt;
    // Point-cloud extent, for XY-disjoint detection. Z is ignored. Empty when no cloud.
    std::optional<Bounds3d> targetExtent;
    std::stop_token stopToken;
    std::function<void(VectorImportProgress)> progress;
};

class VectorImportError : public std::runtime_error { using std::runtime_error::runtime_error; };
class VectorImportLimitExceeded final : public VectorImportError { /* ... */ };
class VectorImportCancelled final : public VectorImportError { /* ... */ };

class VectorLoader {
public:
    virtual ~VectorLoader() = default;
    [[nodiscard]] virtual VectorImportPreflight
    inspect(const VectorImportRequest &) const = 0;
    // Used exactly once when a selected extent union cannot be computed. Visits
    // keys in order and returns the first finite X/Y coordinate.
    [[nodiscard]] virtual std::array<double, 2>
    probeOrigin(const VectorImportRequest &,
                std::span<const VectorSublayerKey>) const = 0;
    // One sublayer per call, so the controller can schedule them independently.
    [[nodiscard]] virtual VectorLayerDataPtr
    loadSublayer(const VectorImportRequest &, VectorSublayerKey) const = 0;
};
```

`OgrVectorLoader` implements it and is **stateless**, so one `const` instance is safe
to call from any number of scheduler workers.

#### GDAL lifetime and threading rules

- `ensureOgrRegistered()` wraps `GDALAllRegister()` in a function-local static — C++
  magic statics give a thread-safe once, and `GDALAllRegister` is not reentrant.
- **One `GDALDataset` per task**, opened and closed inside the task, never a member
  and never shared. `GDALDataset` and `OGRLayer` are not thread-safe. The moment
  somebody caches a dataset "for performance", concurrent imports corrupt each other —
  which is exactly why the loader holds no state, and why per-sublayer scheduling
  (§6.5) opens N datasets rather than sharing one.
- **Never call `GDALDestroyDriverManager()`.** It races with any worker still inside
  GDAL. Consequence: a future ASAN/LSAN lane must *suppress* GDAL driver-manager
  allocations rather than "fix" them.
- **Never call `CPLSetConfigOption`** — it is process-global. Use
  `CPLSetThreadLocalConfigOption`.
- `OgrErrorScope` is a scoped, thread-local `CPLPushErrorHandler` that turns driver
  diagnostics into the thrown exception's message instead of stderr noise. This
  matters for failures like a shapefile missing its `.shx`, where the user needs to
  know *which* sidecar is missing.

#### `inspect()`

Opens with `GDAL_OF_VECTOR | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR`, then per
sublayer records the enumeration index, description, `OGRGeometryTypeToName` label,
`wkbFlatten`-mapped kind, `wkbHasZ`, the WKT (`exportToWkt` then `CPLFree`), and the
extent. Two hard rules:

- **`GetFeatureCount(FALSE)` — never force.** A forced count on GML, DXF, or CSV is a
  full scan, and inspection runs at `PointTaskPriority::Inspection`. `-1` must be
  tolerated all the way through the picker UI.
- **`GetExtent(&env, FALSE)`, and check the returned `OGRErr`** — it is
  `warn_unused_result` and `-Werror` is on in `ci` (§3).

#### `loadSublayer()`

Iterates features with `OGRFeatureUniquePtr`, polling cancellation as specified in
§6.3 and reporting progress every 8192 features. Per feature:

- Null or `IsEmpty()` geometry → `++skippedFeatures`, continue. Real SHP and DXF data
  always contains some.
- `if (geometry->hasCurveGeometry(TRUE))`: first recursively count the source curve's
  control points and reject above `maximumCurveControlPoints`; only then call
  `getLinearGeometry(limits.curveMaximumAngleStepDegrees)` (default **4 degrees
  maximum angular step per generated chord**) and wrap the result in a `unique_ptr`.
  Recursively count the generated points immediately afterward and reject above
  `maximumLinearizedCurvePoints` before copying any coordinate into the builder.
  The generated object remains GDAL-owned and is outside the application working-byte
  counter; like feature materialisation, its allocation occurs before the post-check
  and cannot be capped byte-for-byte. This covers `CircularString`, `CompoundCurve`,
  `CurvePolygon`, `MultiCurve`, and `MultiSurface`, all reachable from GPKG and DXF.
- Dispatch on `wkbFlatten(getGeometryType())`: `wkbPoint` → marker; `wkbLineString` →
  open line string; **`wkbLinearRing` → closed** line string; `wkbPolygon`/`wkbTriangle`
  → exterior plus `getNumInteriorRings()` holes; the four collection types → recurse,
  **bailing at depth 8** against pathological nesting;
  `wkbPolyhedralSurface`/`wkbTIN` → `OGRGeometryFactory::forceToMultiPolygon` on a
  clone, then recurse once; anything else → `++skippedParts`.
- Z is read and discarded, setting `summary.sourceHadZ` so the Inspector can say
  "Z flattened" rather than misleading the user.
- `featureCount` increments **once per top-level feature**, outside recursion.

#### CRS and extent safety

"No reprojection" is a reasonable v1 boundary; "no CRS protection" is not.

- When **both** `targetSpatialReferenceWkt` and the layer's WKT are non-empty, compare
  with `OGRSpatialReference::IsSame()` and set `crsMismatch`. The comparison stays
  inside the GDAL-facing layer, so no PROJ types leak upward. The UI warns; it never
  blocks the load.
- Because the test tile reports **no** SRS, WKT comparison can never fire on the most
  common input. So also set `extentDisjointXY` when the layer and target X/Y
  intervals do not overlap; **ignore Z entirely**, because all planar overlays have
  geometry Z = 0 and style placement is independent. This is the check that catches
  the real failure: a mis-placed layer would otherwise make the camera span half the
  planet and destroy framing. The layer is retained but the UI adds it hidden by
  default (§6.6/§6.9).

#### Ownership rules that will otherwise leak or double-free

`GetNextFeature()` returns an **owned** `OGRFeature*` that must go through
`OGRFeature::DestroyFeature` — plain `delete` is wrong across a DLL boundary on
Windows, so always use `OGRFeatureUniquePtr`. `GetGeometryRef()` is **borrowed**.
`getLinearGeometry()`, `clone()`, and `forceToMultiPolygon()` are **owned**.

### 6.5 Async loading

#### One logical job

The earlier draft had `inspect()` and `load()` each mint a job id, so the inspection
row stayed parked in `AwaitingChoice` forever while a second row did the loading.
One job, one row:

```cpp
enum class VectorLoadJobPhase : std::uint8_t {
    Queued, Inspecting, AwaitingChoice, ResolvingOrigin, Reading,
    Ready, Failed, Cancelled,
};

// src/vector/VectorImport.h; no Qt dependency.
struct VectorSublayerFailure {
    VectorSublayerKey key;
    std::string message;
    bool operator==(const VectorSublayerFailure &) const = default;
};

// Pure C++; keys and failures remain in selected/preflight order.
struct VectorLoadSummary {
    std::vector<VectorSublayerKey> selected;
    std::vector<VectorSublayerKey> successful;
    std::vector<VectorSublayerFailure> failed;

    [[nodiscard]] bool allSucceeded() const noexcept;
    [[nodiscard]] bool partialSuccess() const noexcept;
};
// Declared with Q_DECLARE_METATYPE beside the controller header for queued delivery:
// VectorSublayerKey, VectorLayerDataPtr, VectorLoadSummary.

class VectorLoadController final : public QObject {
    Q_OBJECT
public:
    VectorLoadController(std::shared_ptr<const VectorLoader> loader,
                         std::shared_ptr<PointTaskScheduler> scheduler,
                         QObject *parent = nullptr);
    ~VectorLoadController() override;          // cancels its own jobs; does NOT waitForIdle

    std::uint64_t startInspection(VectorImportRequest request);
    bool continueLoad(std::uint64_t jobId, std::vector<VectorSublayerKey> selected);
    void cancel(std::uint64_t jobId);
    void cancelAll();
    [[nodiscard]] bool retry(std::uint64_t jobId);
    [[nodiscard]] bool hasActiveJobs() const noexcept;
    [[nodiscard]] std::vector<LoadJobRow> jobRows() const;   // typed key, see 6.9
    [[nodiscard]] bool dismiss(std::uint64_t jobId);
    [[nodiscard]] const PointTaskScheduler *schedulerIdentity() const noexcept;

signals:
    void inspected(quint64 jobId, pci::VectorImportPreflight preflight);
    void progressChanged(quint64 jobId, quint64 processed, quint64 total);
    void sublayerLoaded(quint64 jobId, pci::VectorLayerDataPtr layer);
    // Nonterminal: sibling tasks continue and the row remains Reading.
    void sublayerFailed(quint64 jobId,
                        pci::VectorSublayerKey key,
                        QString message);
    // The single normal terminal signal, for complete and partial success.
    void finished(quint64 jobId, pci::VectorLoadSummary summary);
    void failed(quint64 jobId, QString message);
    void cancelled(quint64 jobId, pci::VectorLoadSummary summary);
    void jobStateChanged(quint64 jobId);
};
```

State progression on **one** row is
`Queued → Inspecting → AwaitingChoice → [ResolvingOrigin] → Reading → Ready`; the
bracketed phase occurs only for an extent-less selection. `cancel()` while the picker
is open also **closes the picker** — the dialog observes `jobStateChanged` — and moves
the job to `Cancelled`. Inspection with zero sublayers transitions to `Failed` and
emits `failed` once; `continueLoad` rejects an empty selection without changing
`AwaitingChoice`.

#### Origin probe, then one task per sublayer

When the request supplies no origin and all selected extents are reported, the
controller uses their union centre. If any selected extent is absent, it first
submits one cancellable `OriginProbe` task using `VectorLoader::probeOrigin`; no
sublayer task may start until it succeeds. The probe's deterministic traversal is
specified in §5. Its result is installed in the copied request so every later loader
call snaps the same X/Y.

Each selected sublayer is submitted as its own scheduler task, all sharing
`fairnessGroup = jobId`. This makes the fairness claim real: a four-sublayer GPKG
cannot starve a single-sublayer import, progress and cancellation become
per-sublayer, and each task owns its own `GDALDataset` as thread safety requires. The
job evaluates its aggregate outcome when its outstanding-task counter reaches zero.
A failed sublayer emits `sublayerFailed` but does not cancel its siblings.

The controller stores progress per selected key. `progressChanged` emits the sum of
processed counts; its `total` is the overflow-checked sum only when every selected
sublayer has a known nonzero total, otherwise 0 (indeterminate). Per-key processed
values are monotonic, so aggregate processed never regresses within an attempt.

Every sublayer load reserves
`limits.maximumApplicationWorkingBytes` for scheduler admission; inspection and the
single origin probe reserve 1 MiB. The scheduler's configured application
working-byte ceiling is at least that vector ceiling and uses the same value when the
default `ImportServices` is built. This intentionally serialises vector-sublayer
memory expansion under the default budget: scheduler safety is preferred over
concurrent OGR/earcut working sets. There is no source-file-size heuristic.
`startInspection` synchronously rejects zero or
`maximumApplicationWorkingBytes > scheduler_->activeByteBudget()` with
`std::invalid_argument`, preventing a permanently inadmissible queued task.

#### Partial success, cancellation, and retry

Each job record explicitly retains, in selected order:

- the selected keys;
- which keys successfully emitted a layer;
- failed keys and their messages;
- the outstanding-task count;
- `cancelled`;
- `terminalEmitted`;
- an attempt generation carried by every task callback.

Terminal resolution is exhaustive:

- all selected keys succeeded → phase `Ready`, emit `finished(jobId, summary)` once;
- some succeeded and some failed → phase `Ready`, preserve emitted layers, put the
  warning details in the row, and emit the same `finished(jobId, summary)` once;
- none succeeded → phase `Failed`, emit the job-level `failed` signal exactly once
  with an aggregate of the per-sublayer messages;
- cancellation → phase `Cancelled`, request stop on only this job and emit
  `cancelled(jobId, summary)` exactly once; layers emitted before cancellation remain in the
  document.

A callback whose generation is stale, or whose job has `cancelled == true`, may only
retire its outstanding count; it must discard the data/error and may not emit
`sublayerLoaded`, `sublayerFailed`, or a second terminal signal. This is the explicit
rule for results that race cancellation.

`retry(jobId)` is available after `Ready` with failures, `Failed`, or `Cancelled`,
once old callbacks have drained. It reuses the preflight and resolved shared origin,
clears cancellation/terminal state and prior errors for retry candidates, increments
the attempt generation, and submits **only** selected keys without a successfully
emitted layer: failed keys plus keys cancelled before emission.
Successful keys are never resubmitted, so the Scene document cannot acquire
duplicates. The Tasks row stays one logical job and its summary always describes all
originally selected keys.

#### `ImportServices` ownership and shutdown

The earlier draft claimed sharing the point-import scheduler prevents contention with
hierarchy decoding. **That is false**: the document builds its own
`hierarchyScheduler_` (`PointCloudDocument.cpp:48-54`). The surviving rationale is
narrower but real — one bounded import pool instead of two, and visibility in
`PointTaskSchedulerMetrics`.

But sharing is unsafe as the code stands. `~PointCloudLoadController` calls
`scheduler_->waitForIdle()` (`PointCloudLoadController.cpp:160`), so destroying either
controller would block on the *other's* work, with member destruction order deciding
whether that work has even been cancelled.

The ownership unit is explicit:

```cpp
struct ImportServices {
    std::shared_ptr<PointTaskScheduler> scheduler;
    std::unique_ptr<PointCloudLoadController> pointCloudController;
    std::unique_ptr<VectorLoadController> vectorController;
};

[[nodiscard]] ImportServices makeImportServices(
    std::shared_ptr<const PointCloudLoader> pointLoader,
    std::shared_ptr<const VectorLoader> vectorLoader,
    std::size_t maximumWorkers = PointTaskScheduler::defaultMaximumWorkers,
    std::uint64_t applicationWorkingByteCeiling =
        PointTaskScheduler::defaultActiveByteBudget);
```

Production construction and every injecting test construct **both** controllers with
that exact scheduler. Both controller classes expose
`schedulerIdentity() == scheduler_.get()`. The injecting `MainWindow` constructor
takes one `ImportServices` value and rejects a null member or either identity mismatch
with `std::invalid_argument`; it never accepts controllers independently.

Each controller destructor cancels and disconnects only its own jobs and stop
sources; neither waits. Scheduler closures must therefore capture loader/request/job
state by value and deliver through a `QPointer`/QObject-context queued callback that
is dropped after receiver destruction—no worker may dereference a raw controller.
`MainWindow::~MainWindow()` performs shutdown explicitly:

1. disconnect UI callbacks and destroy/reset both controllers;
2. call `services.scheduler->waitForIdle()` exactly once;
3. destroy/reset the scheduler and then the `ImportServices` holder.

This modifies existing point-cloud shutdown and gets its own commit and its own test
(`MainWindowTests`: both kinds in flight are cancelled, one owner wait joins all
callbacks, and scheduler destruction is last).

### 6.6 Document integration

**Vector layers live in the same document object and share one id space.** The
decisive argument is the id space, not plumbing convenience:
`RenderViewport::frameLayer` takes a `PointCloudLayerId` (`RenderViewport.h:51`),
`PointCloudLayerPanel::currentLayerId()` returns one (`PointCloudLayerPanel.h:51-52`),
every panel callback is typed on it (`:31-42`), and MainWindow has seven handlers
keyed on it. A separate id space needs a parallel twin of each.

Generalising `PointCloudLayer` into a variant was rejected: it is consumed in tight
per-frame loops (`RenderViewportWidget.cpp:743-770`, `refreshSceneSnapshots`,
`refreshSceneSubscriptions`, `UploadScheduler`), and forcing `std::visit` into the hot
path touches ~19 sites in `RenderViewportWidget.h` alone. Two homogeneous vectors are
strictly better — the point pipeline never sees a vector layer.

```cpp
enum class SceneLayerKind : std::uint8_t { None = 0, PointCloud = 1, Vector = 2 };

using SceneLayerId = std::uint64_t;
// Transitional spelling. New code uses SceneLayerId; the point-cloud pipeline
// still reads naturally as PointCloudLayerId.
using PointCloudLayerId = SceneLayerId;

struct VectorLayer {
    SceneLayerId id = 0;
    VectorLayerDataPtr data;
    bool visible = true;
    VectorLayerStyle style;
};
```

New API, following the existing `[[nodiscard]] bool` mutator convention
(`PointCloudDocument.h:63-72`):

```cpp
[[nodiscard]] std::size_t vectorLayerCount() const noexcept;
[[nodiscard]] SceneLayerId addVectorLayer(VectorLayerDataPtr data,
                                          bool initiallyVisible = true);
[[nodiscard]] std::optional<VectorLayer> vectorLayer(SceneLayerId id) const;
[[nodiscard]] std::vector<VectorLayer> vectorLayers() const;
[[nodiscard]] bool setVectorLayerStyle(SceneLayerId id, VectorLayerStyle style);
[[nodiscard]] std::uint64_t vectorRevision() const noexcept;

[[nodiscard]] SceneLayerKind layerKind(SceneLayerId id) const noexcept;
[[nodiscard]] bool hasAnyLayer() const noexcept;
[[nodiscard]] std::optional<Bounds3d> sceneBounds() const;
[[nodiscard]] std::optional<Bounds3d> visibleSceneBounds() const;
[[nodiscard]] std::optional<Bounds3d> layerBounds(SceneLayerId id) const;

// Dispatch on kind. Point ids bump revision(); vector ids bump vectorRevision().
[[nodiscard]] bool removeLayer(SceneLayerId id);
[[nodiscard]] bool setLayerVisible(SceneLayerId id, bool visible);
// New: these centralise kind dispatch so MainWindow's loops disappear.
[[nodiscard]] bool isolateLayer(SceneLayerId id);
[[nodiscard]] bool setAllLayersVisible(bool visible);

// Copies (never moves) vector records, preserving ids, and advances nextLayerId_
// past every copied id. See the rollback discussion below.
[[nodiscard]] bool copyVectorLayersFrom(const SceneDocument &source);
```

Five invariants carry real weight:

1. **A separate `vectorRevision_`.** `revision()` feeds `FlatFramePlanKey`
   (`RenderViewportWidget.h:156-165`), the full-detail plan (`:1220`, `:1335`), the
   selection generation (`:2254`), and the measurement hover cache (`:2449`). Dragging
   an opacity slider must not invalidate any of that. **Do not** add a combined
   `sceneRevision()` that sums the two — sums alias (0 + 3 == 1 + 2).
2. **`bounds()` stays point-cloud-only.** Its comment at `PointCloudDocument.h:76-78`
   records that automatic X/Y/Z colour normalisation depends on it. Folding vector
   extents in would silently shift point colours. `sceneBounds()` is for camera
   framing and nothing else.
3. **`empty()` stays point-cloud-only.** It is the replace-or-add predicate at ten
   MainWindow sites. Widening it makes a vector-only scene prompt "A point-cloud scene
   is already open." `hasAnyLayer()` is the new scene-wide predicate.
4. **Degenerate-bounds guard.** A single-point layer produces a zero-extent box, and
   `Bounds3d::maximumExtent()` feeding `frameBounds` would yield a zero camera
   distance. Inflate any zero-extent axis by ±0.5 m before returning.
5. **`isolateLayer` and `setAllLayersVisible` belong on the document.** Shared ids do
   *not* make every existing handler work unchanged — `MainWindow::isolateLayer` and
   `showAllLayers` iterate `document_->layers()` only (`MainWindow.cpp:1175-1187`).
   Moving both to the document keeps kind dispatch in one place and collapses each
   MainWindow handler to a single call that bumps whichever revisions changed.

An imported layer with `data->extentDisjointXY` is admitted with
`addVectorLayer(data, false)`. It remains a normal retained Scene layer, but
`visibleSceneBounds()` excludes it, so the existing point-cloud framing and Fit Scene
remain stable. The warning's explicit **Show anyway** action calls
`setLayerVisible(id, true)`; only then may its X/Y extent enter
`visibleSceneBounds()`. A normal visibility checkbox change is equally explicit and
has the same effect.

`sceneBounds()` reuses the file-local `extendBounds()` already at
`PointCloudDocument.cpp:11`, as `bounds()` and `visibleBounds()` do at `:250` and
`:270`. No new bounds utility.

#### Replacement must preserve rollback

`load.previousDocument = document_` (`MainWindow.cpp:1553`) is retained so a failed or
cancelled point import can roll back. **Moving** overlays out of the old document
would restore a document stripped of them — hence `copyVectorLayersFrom`, which is
cheap because the geometry is already `shared_ptr<const>`.

Replacement sequence:

1. Retain the old document unchanged.
2. Copy vector records into the new document, preserving ids.
3. Advance `nextLayerId_` beyond every copied id.
4. Add the new point layer.
5. On failure, restore the untouched old document.

Tests cover single replace, batch replace, cancellation after preview admission, and
all-inputs-failed.

### 6.7 Render pass restructure

The one invasive change to existing rendering code. Its blast radius is smaller than
it looks: `.draw(` and `.composite(` have exactly **four** call sites, all inside
`RenderViewportWidget::render` (`:866`, `:870`, `:887`, `:891`), and no test calls
either — `tests/qt/PointCloudRendererTests.cpp` includes the headers only to exercise
pure functions. So **rename, do not wrap.** Compatibility wrappers would be dead code
carrying a second `beginPass` that clears — the kind of drift that produced the
clear-colour literal duplicated at `PointCloudRenderer.cpp:182` and
`EyeDomeLightingPass.cpp:158`. Hoist that into one `sceneClearColorRgba` constant.

**The EDL split is forced, not stylistic.** `EyeDomeLightingPass::composite` calls
`rhi_->nextResourceUpdateBatch()` and `commandBuffer->resourceUpdate(updates)` at
`EyeDomeLightingPass.cpp:147-150`, *before* its `beginPass`. A resource-update batch
cannot be submitted inside an open pass, so it must split into
`updateUniforms(cb, near, far)` and `recordComposite(cb, target)`. That also makes EDL
match the `updateUniforms` / `draw` shape `PointCloudRenderer` already has.

```cpp
// New private helper, collapsing the duplicated blocks at
// RenderViewportWidget.cpp:861-875 and :882-896 into one.
void RenderViewportWidget::recordScene(
    QRhiCommandBuffer *commandBuffer,
    const std::vector<BlockDraw> &draws,
    const std::vector<VectorLayerDraw> &vectorDraws)
{
    const QColor clear = QColor::fromRgbF(
        sceneClearColorRgba[0], sceneClearColorRgba[1],
        sceneClearColorRgba[2], sceneClearColorRgba[3]);
    constexpr QRhiDepthStencilClearValue depthClear{1.0F, 0};

    QRhiRenderTarget *pointTarget = eyeDomeLightingActive_
        ? eyeDomeLightingPass_.pointRenderTarget()
        : renderTarget();

    commandBuffer->beginPass(pointTarget, clear, depthClear);
    pointCloudRenderer_.recordDraws(commandBuffer, pointTarget, draws);
    if (!eyeDomeLightingActive_) {
        vectorLayerRenderer_.recordDraws(commandBuffer, pointTarget, vectorDraws);
    }
    commandBuffer->endPass();

    if (eyeDomeLightingActive_) {
        commandBuffer->beginPass(renderTarget(), clear, depthClear);
        eyeDomeLightingPass_.recordComposite(commandBuffer, renderTarget());
        vectorLayerRenderer_.recordDraws(commandBuffer, renderTarget(), vectorDraws);
        commandBuffer->endPass();
    }
}
```

Call order in `render()` — **every** resource update precedes the first `beginPass`:

```cpp
refreshVectorLayers(commandBuffer);            // release/upload sweep, top of frame
pointCloudRenderer_.updateUniforms(commandBuffer, draws);
vectorLayerRenderer_.updateUniforms(commandBuffer, vectorDraws);
if (eyeDomeLightingActive_) {
    const auto clip = camera_.clipPlanes();
    eyeDomeLightingPass_.updateUniforms(commandBuffer,
        static_cast<float>(clip.nearPlane), static_cast<float>(clip.farPlane));
}
submitPendingPick(commandBuffer, draws);       // still owns its offscreen pass
recordScene(commandBuffer, draws, vectorDraws);
```

The no-document branch becomes `recordScene(commandBuffer, {}, {})`. Preserve today's
property that recording an empty list touches nothing — that path never calls
`updateUniforms`, so the UBO may be unwritten. Add
`vectorLayerRenderer_.releaseResources();` to
`RenderViewportWidget::releaseResources()` (`:2727-2734`).

Vectors always draw into whichever pass targets `renderTarget()`, so they land
**after** EDL. A styled colour is therefore the colour on screen, identical whether
depth enhancement is on or off — which matters, because EDL darkens depth
discontinuities and would otherwise halo every stroke.

For the depth test to work in the EDL path, the widget's depth buffer must carry point
depth. `shaders/edl.frag` gains one line:

```glsl
    float centerDepth = texture(sceneDepth, uv).r;
    // Republish the offscreen point depth into this pass's depth attachment so
    // overlays recorded after the composite can depth-test against the cloud.
    // SPIR-V leaves the depth output UNDEFINED on any path that skips this store,
    // so it must precede the early return below.
    gl_FragDepth = centerDepth;
```

Placing it **immediately after sampling `centerDepth` and before every early return**
(including `centerDepth >= 1.0` at `edl.frag:39-42`) is a correctness requirement,
not tidiness. The EDL pipeline switches to
`depthTest(true) / depthOp(Always) / depthWrite(true)` — `Always` rather than no test,
because of the backend divergence documented in §3.

**Rejected alternatives**, each checked against Qt 6.11:

| Option | Verdict |
|---|---|
| `PreserveDepthStencilContents` on the widget target | Flags are baked at `create()`, and `QRhiWidgetPrivate` owns and recreates the target. Cannot be set; would not survive a resize. |
| `setAutoRenderTarget(false)` + one shared depth texture on both targets | Pass 2 would sample that texture while it is the current pass's depth attachment. Vulkan needs `DEPTH_STENCIL_READ_ONLY_OPTIMAL`; QRhi has no API for it. |
| A third pass sampling the EDL depth texture, `discard`-ing manually | Needs a second set of vector pipelines: the non-EDL path has no depth *texture* — the widget's is a `QRhiRenderBuffer` (`qrhiwidget_p.h:58`). |
| Draw vectors into the EDL offscreen target | EDL would shade the fills, and "always on top" becomes impossible. |

**A simplification falls out.** Vector pipelines are built against
`renderTarget()->renderPassDescriptor()` and *only* that one — in both EDL states,
since EDL-off pass 1 and EDL-on pass 2 both target `renderTarget()`. They therefore
have no ordering constraint against `eyeDomeLightingPass_.releaseResources()` and
never need rebuilding on an EDL toggle, unlike `pointCloudRenderer_`
(`RenderViewportWidget.cpp:632-651`).

**Extract a shared `FrameCamera` first.** `buildFramePlan` (`:1493-1510`) and
`buildDrawList` (`:2073-2108`) already rebuild projection and view independently;
`buildVectorDrawList` would be a third copy.

```cpp
struct FrameCamera {
    QMatrix4x4 viewProjection;   // clipSpaceCorr * projection * view, eye at origin
    FrustumCuller culler;
    Vec3d eye;
    Vec3d forward;
    QSize outputSize;
    double nearPlane = 0.0;
    double farPlane = 0.0;
    bool orthographic = false;
    double orthographicScale = 0.0;
    double verticalFovDegrees = 0.0;

    // Orthographic: orthographicScale / outputSize.height(), depth ignored.
    // Perspective:  2 * tan(fov/2) * depth / outputSize.height().
    [[nodiscard]] double worldUnitsPerPixelAtDepth(double depth) const noexcept;
    // nearPlane for perspective; 0.0 for orthographic, where w is always 1.
    [[nodiscard]] float shaderNearPlaneW() const noexcept;
};
```

The earlier draft carried a `worldUnitsPerPixelAtUnitDepth` that was **zero for
orthographic** — wrong twice over, since ortho has a *constant nonzero* scale and a
bare scalar is meaningless for perspective without a depth. The method form fixes
both.

### 6.8 Vector pipelines and shaders

**One uniform per layer, not per primitive.** Carrying fill, stroke, and marker
colours lets one 144-byte record drive every draw of a layer, and lets the marker
shader draw a filled *and* stroked marker in one pass without conflating the marker
style with polygon fill.

```cpp
struct alignas(16) VectorLayerUniform {   // std140, frozen ABI in the house style
    float mvp[16]{};                       //   0
    float fillColor[4]{};                  //  64
    float strokeColor[4]{};                //  80
    float markerColor[4]{};                //  96
    float viewportPixels[2]{};             // 112  device pixels of the output target
    float strokeHalfWidthPixels = 0.5F;    // 120
    float markerHalfSizePixels = 3.0F;     // 124
    float opacity = 1.0F;                  // 128
    std::int32_t markerShape = 0;          // 132  0 = circle, 1 = square
    float featherPixels = 1.0F;            // 136  analytic AA width
    float nearPlaneW = 0.0F;               // 140  camera near distance; 0 for ortho
};
static_assert(std::is_standard_layout_v<VectorLayerUniform>);
static_assert(offsetof(VectorLayerUniform, fillColor) == 64);
static_assert(offsetof(VectorLayerUniform, strokeColor) == 80);
static_assert(offsetof(VectorLayerUniform, markerColor) == 96);
static_assert(offsetof(VectorLayerUniform, viewportPixels) == 112);
static_assert(offsetof(VectorLayerUniform, strokeHalfWidthPixels) == 120);
static_assert(offsetof(VectorLayerUniform, markerHalfSizePixels) == 124);
static_assert(offsetof(VectorLayerUniform, opacity) == 128);
static_assert(offsetof(VectorLayerUniform, markerShape) == 132);
static_assert(offsetof(VectorLayerUniform, featherPixels) == 136);
static_assert(offsetof(VectorLayerUniform, nearPlaneW) == 140);
static_assert(sizeof(VectorLayerUniform) == 144);
```

A **separate** struct and bindings from `BlockUniform` — that 144-byte layout is a
frozen ABI whose `reserved[4]` slot is live in `points.vert:43-63`. `zOffset` is
deliberately **not** a uniform: it is folded into the model translation on the CPU in
double, so the vertex shaders can write `vec4(position, 0.0, 1.0)`. Never put a GLSL
`bool` in a UBO — `markerShape` is an `int`, matching `colorSource` at `points.vert:11`.

One `QRhiShaderResourceBindings` is shared by all six pipelines:
`uniformBufferWithDynamicOffset(0, VertexStage | FragmentStage, uniformBuffer_, sizeof(VectorLayerUniform))`.
No textures. Growth reuses `grownUniformDrawCapacity` (`PointCloudRenderer.h:36-38`)
with an initial capacity of 64.

#### Remaining renderer types

```cpp
// Per-layer GPU residency. Keyed on BOTH the layer id and the data pointer, so a
// reused id with different data can never serve a stale buffer.
struct VectorLayerGpuBuffers {
    struct FillBatchBuffers {
        QRhiBuffer *vertices = nullptr;
        QRhiBuffer *indices = nullptr;      // always IndexUInt16
        quint32 indexCount = 0;
    };
    std::vector<FillBatchBuffers> fillBatches;
    QRhiBuffer *lineSegments = nullptr;
    QRhiBuffer *markerPoints = nullptr;
    quint32 lineSegmentCount = 0;
    quint32 markerCount = 0;
    VectorLayerDataPtr data;                // identity, not just a revision number
    std::uint64_t byteCount = 0;
};

// One record per visible layer, in painter order. `buffers` is BORROWED from the
// renderer's layer map and must not outlive the frame.
struct VectorLayerDraw {
    SceneLayerId layerId = 0;
    const VectorLayerGpuBuffers *buffers = nullptr;
    bool alwaysOnTop = false;
    quint32 uniformIndex = 0;
    VectorLayerUniform uniform;
};

enum class VectorPrimitive : std::size_t { Fill = 0, Line = 1, Marker = 2 };
enum class VectorDepthMode : std::size_t { Tested = 0, AlwaysOnTop = 1 };

// Pure; unit-tested. Maps uniquely onto the six pipeline slots.
[[nodiscard]] constexpr std::size_t
vectorPipelineIndex(VectorPrimitive primitive, VectorDepthMode mode) noexcept
{
    return static_cast<std::size_t>(primitive) * 2U + static_cast<std::size_t>(mode);
}

// Pure; unit-tested. Mirrors stageBlockUniforms (PointCloudRenderer.h:81).
[[nodiscard]] std::vector<std::byte> stageVectorLayerUniforms(
    std::span<const VectorLayerDraw> draws, std::size_t uniformStride);
```

**Six pipelines** — 3 primitives × 2 depth modes, because QRhi has no dynamic depth
state:

| | fill | line | marker |
|---|---|---|---|
| topology | `Triangles` | `TriangleStrip` | `TriangleStrip` |
| vertex input | binding 0 `PerVertex`, stride 8, `Float2` | binding 0 `PerInstance`, stride 16, `Float4` | binding 0 `PerInstance`, stride 8, `Float2` |
| draw | `drawIndexed(indexCount)` per batch | `draw(4, segmentCount)` | `draw(4, markerCount)` |

Shared state: `setCullMode(None)` (earcut winding is unspecified and screen-space
quads have no meaningful winding), `setDepthWrite(false)` always, and `TargetBlend`
with only `enable = true` — QRhi's defaults are already premultiplied-over, so the
fragment shaders emit premultiplied colour. Depth mode `Tested` uses
`setDepthTest(true)` with **`LessOrEqual`, not `Less`**, so a footprint sitting exactly
at ground-sample depth is not annihilated by the D24 requantisation of the republished
EDL depth. `AlwaysOnTop` uses `setDepthTest(false)`.

**No corner vertex buffer.** `gl_VertexIndex` generates the quad corners, as
`shaders/edl.vert:5-9` already does for its fullscreen triangle.

**Index width: triangle-stream partitioning, not a feature probe.** `uint16` addresses
65,536 vertices (0…65535). The builder applies the original-to-local remap algorithm
in §6.3 to earcut's triangle stream, including a single connected polygon above the
threshold; it never defers an entire polygon to a new batch. Fill batches therefore
stay at ≤ 65,536 local vertices and always use 16-bit indices.
`QRhi::ElementIndexUint` is never needed and no `fillIndicesAre32Bit` flag exists.
Draw calls per layer become `fillBatches.size() + 2`, bounded by
`maximumEmittedFillVertices` and `maximumFillIndices`. Boundary duplication is
visible to both the emitted-vertex and retained-byte counters.

**Missing instancing is a visible error, never a silent one.** The earlier draft
proposed silently disabling overlays, which produces a successful import, a Scene row,
and no pixels — the worst possible outcome. A CPU quad-expansion fallback is not worth
v1 (it triples vertex memory for a case no supported backend hits: Metal, Vulkan,
D3D11+ and GL ES 3 all have instancing, and the viewport already hard-fails without
`VertexShaderPointSize`, which is *rarer*). The renderer-facing interface adds:

```cpp
enum class VectorOverlayCapability : std::uint8_t {
    Unknown,
    Supported,
    Unsupported,
};
using VectorOverlayCapabilityCallback =
    std::function<void(VectorOverlayCapability, const QString &reason)>;

[[nodiscard]] virtual VectorOverlayCapability
vectorOverlayCapability() const noexcept = 0;
virtual void setVectorOverlayCapabilityCallback(
    VectorOverlayCapabilityCallback callback) = 0;
```

`RenderViewportWidget` starts at `Unknown`, probes `QRhi::Instancing` once in
`ensureResources`, stores `Supported` or `Unsupported`, and invokes the callback on
the GUI thread when the value changes. `MainWindow` keeps vector import disabled
while the capability is `Unknown` **or** `Unsupported`; the latter uses the callback
reason as its tooltip and marks any already-loaded layer unsupported in its Scene row
and Inspector. Test viewports choose their capability explicitly, so UI tests cannot
accidentally bypass the gate.

#### The `w = 1` pre-divide, which replaces `noperspective`

Each screen-space quad emits `gl_Position = vec4(pixel / halfViewport, ndcZ, 1.0)` —
XY and Z already divided, `w = 1`. All four corners then share `w`, so every varying
interpolates **affinely**, which is exactly what a screen-space quad needs. This drops
the dependency on `noperspective` (patchy in ESSL, extra SPIRV-Cross surface area),
and linear-in-screen interpolation of NDC z between endpoints is precisely what the
rasteriser would do for a real 3D line. `clipSpaceCorrMatrix()` is already folded into
`mvp`, so `clip.z / clip.w` is already the backend's NDC depth.

*(`noperspective` was measured to work — §3 — so it remains a viable fallback. The
pre-divide is cheaper and has less to go wrong.)*

**No Y-flip is needed**, despite `QRhi::isYUpInNDC()` differing per backend. Every
offset is symmetric (`±normal`, `±pad`, radial), so flipping Y yields the identical
point set, and `cullMode(None)` makes winding irrelevant. All distance maths happens
in one self-consistent space (`ndc.xy * halfViewport`) shared by both stages;
`gl_FragCoord` is never used, so its origin convention never enters.

#### `shaders/vector_line.vert`

```glsl
#version 450

layout(location = 0) in vec4 segment;   // x0, y0, x1, y1 relative to the origin

layout(std140, binding = 0) uniform VectorLayerData {
    mat4 mvp;
    vec4 fillColor;
    vec4 strokeColor;
    vec2 viewportPixels;
    float strokeHalfWidthPixels;
    float markerHalfSizePixels;
    float opacity;
    int markerShape;
    float featherPixels;
    float nearPlaneW;
} layerData;

layout(location = 0) out vec2 pixelPosition;
layout(location = 1) flat out vec4 pixelSegment;   // p0.xy, p1.xy in pixels

void main()
{
    vec4 clip0 = layerData.mvp * vec4(segment.xy, 0.0, 1.0);
    vec4 clip1 = layerData.mvp * vec4(segment.zw, 0.0, 1.0);

    // clip.w IS view-space depth (the clip-space correction matrix leaves w
    // untouched), so comparing against the camera's near distance is the real
    // near-plane test. Comparing against a small epsilon would only find the EYE
    // plane and would screen-expand points between the eye and the near plane into
    // enormous quads with unstable direction vectors.
    float nearW = layerData.nearPlaneW;

    if (clip0.w < nearW && clip1.w < nearW) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);   // outside every clip volume
        pixelPosition = vec2(0.0);
        pixelSegment = vec4(0.0);
        return;
    }
    if (clip0.w < nearW) {
        clip0 = mix(clip0, clip1, (nearW - clip0.w) / (clip1.w - clip0.w));
    } else if (clip1.w < nearW) {
        clip1 = mix(clip1, clip0, (nearW - clip1.w) / (clip0.w - clip1.w));
    }

    vec3 ndc0 = clip0.xyz / clip0.w;
    vec3 ndc1 = clip1.xyz / clip1.w;

    vec2 halfViewport = layerData.viewportPixels * 0.5;
    vec2 p0 = ndc0.xy * halfViewport;
    vec2 p1 = ndc1.xy * halfViewport;

    vec2 delta = p1 - p0;
    float span = length(delta);
    vec2 direction = span > 1e-6 ? delta / span : vec2(1.0, 0.0);
    vec2 normal = vec2(-direction.y, direction.x);

    // Padding leaves room for antialiasing and for round caps.
    float pad = layerData.strokeHalfWidthPixels + layerData.featherPixels;

    // Strip order 0..3 -> (start,-) (start,+) (end,-) (end,+).
    bool atEnd = (gl_VertexIndex & 2) != 0;
    float side = (gl_VertexIndex & 1) != 0 ? 1.0 : -1.0;

    vec2 pixel = (atEnd ? p1 : p0)
        + direction * (atEnd ? pad : -pad)
        + normal * side * pad;

    gl_Position = vec4(pixel / halfViewport, atEnd ? ndc1.z : ndc0.z, 1.0);
    pixelPosition = pixel;
    pixelSegment = vec4(p0, p1);
}
```

For orthographic projection `nearPlaneW` is 0.0: `w` is always 1, no perspective
divide can blow up, and the test never fires. `flat` on `pixelSegment` is safe despite
provoking-vertex differences, because all four vertices of an instance write the same
value.

#### `shaders/vector_line.frag` — the capsule that deletes a draw call

```glsl
#version 450

layout(location = 0) in vec2 pixelPosition;
layout(location = 1) flat in vec4 pixelSegment;

layout(std140, binding = 0) uniform VectorLayerData { /* identical block */ } layerData;

layout(location = 0) out vec4 fragmentColor;

float distanceToSegment(vec2 point, vec2 from, vec2 to)
{
    vec2 span = to - from;
    float lengthSquared = dot(span, span);
    float t = lengthSquared > 1e-12
        ? clamp(dot(point - from, span) / lengthSquared, 0.0, 1.0)
        : 0.0;
    return length(point - (from + span * t));
}

void main()
{
    // A capsule. Clamping t to [0,1] gives round caps, and the capsules of
    // consecutive segments overlap at shared vertices, giving round joins.
    float distance = distanceToSegment(
        pixelPosition, pixelSegment.xy, pixelSegment.zw);
    float feather = max(layerData.featherPixels, 1e-3);
    float coverage = clamp(
        (layerData.strokeHalfWidthPixels - distance) / feather + 0.5, 0.0, 1.0);
    if (coverage <= 0.0) {
        discard;
    }
    float alpha = layerData.strokeColor.a * layerData.opacity * coverage;
    fragmentColor = vec4(layerData.strokeColor.rgb * alpha, alpha);
}
```

Round joins, round caps, and analytic antialiasing all fall out of this one SDF for
one `flat vec4` and a dozen ALU ops — removing the separate instanced joint-disc pass
an earlier draft carried, and halving both instance count and per-layer buffer count.

#### The other four shaders

`vector_fill.vert` is `gl_Position = layerData.mvp * vec4(position, 0.0, 1.0);`.
`vector_fill.frag` emits the premultiplied fill colour scaled by `opacity`.

`vector_marker.vert` projects the centre with the **same `nearPlaneW` test**, expands
by `markerHalfSizePixels + strokeHalfWidthPixels + featherPixels` using
`gl_VertexIndex` corners, and passes `vec2 localPixel` (pixels from the centre).

`vector_marker.frag` computes a signed distance to the outline —
`length(localPixel) - halfSize` for a circle, `max(abs(x), abs(y)) - halfSize` for a
square — then derives `outer` and `inner` coverages straddling the outline, so a
marker is filled *and* stroked in one pass. `markerShape` is a uniform, so the branch
is uniform control flow with no divergence cost.

#### Build inventory

Append all six files to the **existing** `qt_add_shaders` call in
`cmake/PciShaders.cmake:9-18` and widen its comment (it currently says
"point-render and pick shader ABI"). Do not add a second function:
`tests/CMakeLists.txt:185-186` calls
`pci_add_point_shaders(pcinspector_gpu_tests test_point_shaders)`, so extending the
single inventory gives the GPU tests the vector shaders for free and preserves the
documented "single build-system inventory" invariant. Renaming it
`pci_add_render_shaders` in the same commit is required; update both production and
test call sites.

#### Per-frame assembly

`buildVectorDrawList(const FrameCamera &)` sits beside `buildDrawList`
(`RenderViewportWidget.cpp:2070`) and reuses the camera-relative construction from §5.
Per visible layer it looks up GPU buffers, culls, builds the model matrix with
`zOffset` folded in, and fills one uniform including
`nearPlaneW = frame.shaderNearPlaneW()`. `alwaysOnTop` layers are emitted after all
depth-tested ones; within each group, document order is painter order.
`recordDraws` must iterate layers in that order and, per layer, record fill batches,
then lines, then markers. It must not regroup all fills/lines/markers across layers,
which would change deterministic painter-order overlap.

Details that are easy to get wrong:

- **Cull bounds must be inflated by the on-screen size of stroke and markers**, or a
  layer whose anchors sit just off screen but whose 8-pixel markers should be visible
  will pop. Compute camera depth
  `dot(corner - frame.eye, frame.forward)` for **all eight** world-space layer-bound
  corners after applying `zOffset`. For perspective, reject the layer immediately
  when the largest corner depth is before the near plane. Otherwise use
  `clamp(largestCornerDepth, frame.nearPlane, frame.farPlane)` — the farthest relevant
  depth in the camera range — with `worldUnitsPerPixelAtDepth`, which conservatively
  covers a depth-spanning layer. Orthographic projection uses its
  constant, nonzero `worldUnitsPerPixelAtDepth` value. The pixel radius is the maximum
  of `strokeHalfWidthPixels + featherPixels` when segments exist and
  `markerHalfSizePixels + strokeHalfWidthPixels + featherPixels` when markers exist.
  Multiply by the selected world-units-per-pixel value, inflate the AABB
  conservatively on all axes, and only then call `FrustumCuller::intersects`.

  ```cpp
  // nullopt means wholly before the perspective near plane or outside the frustum.
  [[nodiscard]] std::optional<Bounds3d> vectorLayerCullBounds(
      const VectorLayerData &geometry,
      const VectorLayerStyle &style,
      const FrameCamera &frame) noexcept;
  ```

  The helper is pure and unit-tested.
  `FrustumCuller::intersects` handles a zero-Z-extent AABB correctly. Culling is per
  layer, so one city-wide layer always intersects — per-feature culling needs a
  spatial index and is a later concern.
- **`setShaderResources` must follow every `setGraphicsPipeline`.** The "skip empty
  primitive groups" loop makes it easy to hoist that call out by accident; keep it
  inside each `if` block.

`VectorLayerRenderer` owns `std::unordered_map<SceneLayerId, VectorLayerGpuBuffers>`,
creating `QRhiBuffer::Immutable` buffers on first sight of a layer and releasing them
when it leaves the document. Do the whole sweep at the **top** of `render()` in one
`QRhiResourceUpdateBatch` — `VectorLayerDraw::buffers` is a borrowed pointer into that
map, so the map must not rehash between `buildVectorDrawList` and `recordDraws`.

**Cache invalidation must include document identity.** Revisions are document-local, so
a new document can carry the same `vectorRevision()` as the old one while holding
different layers — which is precisely why `setDocument` already resets
`seenDocumentRevision_ = 0` (`RenderViewportWidget.cpp:334`). So `setDocument` must
also reset `seenVectorRevision_` and reconcile or release vector buffers, and the
per-layer record keys on the **`VectorLayerDataPtr` as well as the id**, so a reused
id with different data cannot serve a stale buffer.

Style-only edits do no GPU work, since the uniform is rebuilt every frame regardless.
`RenderActivity` needs no new flag: uploads are synchronous and complete within the
frame, so idle frames stay free.

`RenderMetrics` gains `vectorLayersDrawn`, `vectorDrawCalls`, and `gpuVectorBytes`.
`drawCalls_` (`RenderViewportWidget.cpp:1073`) currently counts point blocks only —
report the vector count separately rather than folding it in, so the existing metric
keeps its meaning.

Vector buffers sit **outside** the 512 MiB point residency budget;
`maximumEmittedFillVertices`, `maximumFillIndices`, and `maximumRetainedBytes` bound
them, and `gpuVectorBytes` makes the total visible. No LOD or tiling in v1.

### 6.9 UI

#### Actions

`File → Import &Vector Layer…` after `Add Point Clouds…`, objectName
`importVectorLayerAction`, shortcut `Ctrl+Shift+V`, and a tooltip built with the
existing `.arg(shortcut.toString(QKeySequence::NativeText))` idiom. The **same
`QAction` instance** goes on the command toolbar. The Scene header keeps one compact
`Add points…` action rather than crowding a 220 px panel with another labelled
button; the menu and command bar satisfy the "one command, many entrances" rule
(`UI_DESIGN_LANGUAGE.md:209`).

The action begins disabled. `MainWindow` enables it only after
`RenderViewport` reports `VectorOverlayCapability::Supported`; it remains disabled
while capability is `Unknown` or `Unsupported`, with an explanatory tooltip in the
latter state (§6.8).

**Cancel Loading stops gating on `loading_` alone.** `requestLoadCancellation()`
currently early-returns unless the point-cloud `loading_` flag is set
(`MainWindow.cpp:999-1002`). Enablement and cancellation become
`loading_ || importServices_.vectorController->hasActiveJobs()`, and each controller
cancels independently, so vector inspection, awaiting-choice, origin resolution, and
reading all contribute.

#### Tasks dock — a typed job key

`kLoadJobIdRole` is a bare `uint64` and the context menu resolves actions with
`std::ranges::find(loadStates_, jobId, &PointCloudLoadJobState::jobId)`
(`PointCloudLayerPanel.cpp:993-996`). Two controllers each counting from zero **will
collide**. Put only pure identity and capability types in
`pcinspector_import_api`'s `src/import/LoadJobKey.h`:

```cpp
enum class LoadJobKind : std::uint8_t { PointCloud, Vector };

struct LoadJobKey {
    LoadJobKind kind = LoadJobKind::PointCloud;
    std::uint64_t id = 0;
    bool operator==(const LoadJobKey &) const = default;
};

struct LoadJobCapabilities {
    bool canCancel = false, canRetry = false;
    bool canPrioritize = false, canDismiss = false;
};
```

The Qt projection belongs in `pcinspector_import_async`'s
`src/import/LoadJobRow.h`, which also applies
`Q_DECLARE_METATYPE(pci::LoadJobKey)` for the item-data role:

```cpp
struct LoadJobRow {
    LoadJobKey key;
    QString title, detail;
    double completion = 0.0;      // 0..1
    bool terminal = false;
    LoadJobCapabilities capabilities;
};
```

`PointCloudLayerPanel` stores `std::vector<LoadJobRow>` and every task callback takes a
`LoadJobKey`. Row capabilities drive both visible row buttons and the duplicate
context menu, so **Prioritize is simply absent for vector rows** — the OGR path has
no equivalent of point preflight reordering. Vector **Retry** follows the
failed/not-yet-emitted rule in §6.5 and never re-emits a successful sublayer.
**Dismiss** removes a terminal row and is rejected for live ones. Both controllers
project their internal state into `LoadJobRow` inside
`pcinspector_import_async`, so neither `QString` nor a UI row leaks into
`pcinspector_import_api`, and the panel stops depending on
`PointCloudLoadJobState` directly.

Vector row detail text is outcome-specific: `Loaded 5 of 5` for full success,
`Loaded 3 of 5 · 2 failed — Retry failed sublayers` for partial success,
`No sublayers loaded · 5 failed` for failure, and
`Cancelled after 2 of 5 · loaded layers retained` for cancellation. A live row shows
`Reading 3 of 5 · 1 failed`; `sublayerFailed` updates it without making it terminal.
Vector `completion` is `processed / total` when the aggregate total is known;
otherwise it is `(successful + failed) / selected`, clamped to [0,1], and every
terminal row is 1.
After cancellation, `canRetry` and `canDismiss` remain false until all discarded
callbacks drain, then `jobStateChanged` exposes both.

#### Scene list

Both kinds in one list: point-cloud layers in document order, then vector layers in
document order. Vector rows reuse the existing checkable item pattern
(`PointCloudLayerPanel.cpp:836-842`) and keep the id in the existing `kLayerIdRole`.
One new role: `kLayerKindRole = Qt::UserRole + 3` — `+1` and `+2` are already
`kCountRole` and `kLoadJobIdRole` (`:43-45`). Vector items **must** set `kCountRole`,
because `LayerItemDelegate` draws it and an unset role leaves a blank or stale badge.

Visibility, remove, and zoom-to reuse the existing callbacks. Isolate and Show All now
route through the document methods added in §6.6.

#### Inspector

The Inspector keeps the existing `pointCloudLayerProperties` and adds
`vectorLayerProperties`; exactly one is visible for the selected Scene row. This
avoids another container while preserving the contextual-page behavior required by
`UI_DESIGN_LANGUAGE.md:318-352`:

| Section | Row | Widget | objectName |
|---|---|---|---|
| `vectorAppearanceSection` | Fill | `VectorColorButton` | `vectorFillColorButton` |
| | Stroke | `VectorColorButton` | `vectorStrokeColorButton` |
| | Stroke width | `QDoubleSpinBox` 0–20, `" px"` | `vectorStrokeWidthSpinBox` |
| | Marker | `VectorColorButton` | `vectorMarkerColorButton` |
| | Marker size | `QDoubleSpinBox` 1–64, `" px"` | `vectorMarkerSizeSpinBox` |
| | Marker shape | `QComboBox` Circle/Square | `vectorMarkerShapeComboBox` |
| | Opacity | `QDoubleSpinBox` 0–100, `" %"` | `vectorOpacitySpinBox` |
| | — | `QPushButton` "Reset appearance" | `vectorResetStyleButton` |
| `vectorPlacementSection` | Elevation offset | `QDoubleSpinBox` ±1e6, 3 dp | `vectorZOffsetSpinBox` |
| | — | `QPushButton` "Place above scene" | `vectorPlaceAboveSceneButton` |
| | — | `QPushButton` "Match scene floor" | `vectorMatchFloorButton` |
| | — | `QCheckBox` "Always draw on top" | `vectorAlwaysOnTopCheckBox` |
| | — | notice label (see below) | `vectorPlacementNotice` |
| `vectorInformationSection` | Features / Geometry / Driver / Size / Reported CRS / Source | `makeValueLabel` + `setValueText` | `vector*Value` |
| | warnings (reported-CRS mismatch and XY-disjoint extent) | notice + inline action | `vectorExtentWarning`, `vectorShowAnywayButton` |

All value labels use the existing `makeValueLabel()` / `setValueText()` helpers so
eliding and selectability match. All spin boxes use `setKeyboardTracking(false)`
(`PointCloudLayerPanel.cpp:540`). Every edit funnels through one private
`applyVectorStyle()`, guarded by the existing `QSignalBlocker` discipline (`:1381-1383`)
so a refresh never re-emits. Sliders emit on `valueChanged`, not `sliderReleased` — a
style edit bumps only `vectorRevision_`, so live feedback costs one uniform write.

`VectorColorButton` is a small `QPushButton` subclass showing an
alpha-checkerboard icon, hexadecimal color, and alpha percentage, with
`setColor(VectorRgba)` / `color()`.

For `extentDisjointXY`, the warning says the layer is hidden because its reported X/Y
extent does not overlap the point-cloud extent and exposes **Show anyway**.
`vectorShowAnywayButton` explicitly makes that layer visible and then requests a
render; it is absent for ordinary hidden layers.

#### Placement, stated honestly

Z = 0 remains the default, as specified. What changes is that its consequence is no
longer left to be discovered, and that the placement actions are honest about what
they do:

- **"Place above scene"** sets Z to scene max Z + 1 % of the Z range. This is the
  action that actually makes a top-down overlay visible, and it is the primary button.
- **"Match scene floor"** sets Z to scene min Z. It is retained as an explicitly
  labelled ground-plane action, with **no claim that it makes the layer visible** — on
  sloping terrain almost every point sits above that plane, so a depth-tested overlay
  stays hidden. An earlier draft claimed the opposite; that was wrong.
- When Z = 0 falls outside the scene's Z range, `vectorPlacementNotice` shows a
  non-modal message — *"This layer is 750 m below the point cloud."* — with inline
  **Place above scene** and **Always on top** actions. The scene Z range is shown
  beside the Z field.

#### Sublayer picker

`VectorSublayerDialog`, modelled on `PointCloudLoadChoiceDialog`, is shown only when a
source exposes more than one sublayer. It names the source and GDAL driver and uses a
checkable `QListWidget` (`vectorSublayerList`), with all rows checked by default,
two-line type/count/dimensionality summaries, **Select all**, and **Clear**. The
selection summary updates the primary button (`Add layer` / `Add N layers`) and
disables it for an empty selection. Selection returns keys in preflight order.
Completion runs through `connect(dialog, &QDialog::finished, …)` and `open()`, so
there is **no nested event loop**. A terminal `jobStateChanged` closes the dialog if
the job is cancelled from Tasks.

#### Flow

`chooseVectorLayers()` → `QFileDialog::getOpenFileNames` with curated common vector
extensions plus **All files** (GDAL's compiled drivers remain the authority) →
`startInspection(request)` with `targetSpatialReferenceWkt` and `targetExtent` filled
from the document when a point cloud exists (never blocking the GUI thread; OGR `Open`
on a network path can take seconds) → on `inspected`: 1 sublayer calls
`continueLoad` directly, >1 opens the picker (zero is already a controller failure) →
each
`sublayerLoaded` calls
`addVectorLayer(layer, !layer->extentDisjointXY)`, frames only if the scene previously
had no visible layer and the new layer is visible, otherwise requests a render →
`sublayerFailed` updates warning status without ending siblings →
`finished(jobId, summary)` reports full or partial success. A job-level `failed`
means no selected sublayer succeeded. Failures go to the status bar and the Tasks
row, never a modal (`UI_DESIGN_LANGUAGE.md:426`).

#### Renderer framing — the change most likely to be forgotten

`RenderViewportWidget.cpp:385-386` and `:395-396` must switch `visibleBounds()` →
`visibleSceneBounds()`, and `frameLayer` at `:440` must use the new
`document_->layerBounds(id)`. Without this, importing into an empty scene leaves the
camera at its default and the user sees a black viewport. `document_->bounds()` at
`:747` for colour normalisation stays **unchanged**.

Also note `pointCount_ = document_->visibleExpectedPointCount()`
(`RenderViewportWidget.cpp:355`) stays 0 for a vector-only scene; audit the frame
path's early-outs so "no points" does not mean "nothing to draw".

#### Testability hook

The injecting constructor accepts the `ImportServices` bundle from §6.5. UI tests
supply fake point/vector loaders but create both real controllers against one test
scheduler; constructor validation proves the tested shutdown and admission topology
is the production topology. Fake `RenderViewport` implementations also expose an
explicit vector capability and callback.

---

## 7. Implementation phases

Each phase compiles, passes its tests, and leaves `main` shippable.

### Phase 0 — `SceneDocument` rename *(implemented; final regression audit pending)*

1. `git mv src/scene/PointCloudDocument.{h,cpp} src/scene/SceneDocument.{h,cpp}`;
   `tests/unit/PointCloudDocumentTests.cpp` → `SceneDocumentTests.cpp`.
2. Rename `PointCloudDocument` → `SceneDocument`, `…Ptr`, `…Metrics`. 88 occurrences
   across 17 files; 32 are method qualifiers in one `.cpp`, 34 in tests. Mechanical
   and fully compiler-verified.
3. Add `using SceneLayerId = std::uint64_t;` keeping
   `using PointCloudLayerId = SceneLayerId;`. **Do not** mass-rename
   `PointCloudLayerId` (~250 occurrences, 93 in `PointCloudLayerPanel.cpp`) for zero
   behavioural gain. Keep `PointCloudLayer` — it genuinely is a point-cloud layer.

**Done when** `ctest --preset ci` is green with no behaviour change. Commit separately
before any vector work. The rename is part of this specification so later public APIs
have one unambiguous document name.

### Phase 1 — Vendored earcut and the geometry module *(implemented core; coverage/provenance pending)*

1. Vendor `third_party/earcut/include/mapbox/earcut.hpp` plus `LICENSE` and a
   `README.md` recording the upstream URL, pinned commit SHA, and date. Not a
   submodule — it is one header.
2. Add the `pcinspector_earcut` INTERFACE target with
   `target_include_directories(... SYSTEM INTERFACE ...)`.
   > **`SYSTEM` is not optional.** earcut.hpp does not survive
   > `-Wall -Wextra -Wpedantic -Werror`, and it must stay byte-identical to upstream.
   > This is the single most likely thing to break the `ci` preset.
3. Write `VectorGeometry.h/.cpp` (split input/emitted/index/retained/working limits,
   incremental enforcement, triangle-stream partitioning with original-to-local
   remaps, duplicate stripping, the area-validation contract, parts/features counters),
   `VectorLayerStyle.h/.cpp`, `VectorLayerData.h/.cpp`, `VectorImport.h`.
4. Add the `pcinspector_vector` target between `pointcloud` and `scene`.
5. Write `tests/unit/VectorGeometryTests.cpp`, `VectorGeometryLimitsTests.cpp`, and
   `VectorLayerStyleTests.cpp`; add `pcinspector::vector` to
   `pcinspector_unit_tests`' `LIBRARIES` (currently only `pcinspector::app_model`,
   `tests/CMakeLists.txt:34-35`).

**Done when** the closed-ring, duplicate-coordinate, hole, bowtie-validation,
connected-polygon batch-boundary, duplication-charged limit, limit-enforcement,
non-finite, and precision tests pass under `ci`.

### Phase 2 — GDAL import *(basic implementation; fixture and hostile-input coverage pending)*

1. `find_package(GDAL 3.4 REQUIRED)` — **module-or-config, not `CONFIG`-only** — in the
   top-level `CMakeLists.txt` beside the PDAL lookup at `:57`, with a comment noting
   both link PROJ and GEOS and must resolve to the same builds.
2. Write `OgrRuntime.h/.cpp` and `OgrVectorLoader.h/.cpp` (§6.4), including the
   sublayer key, deterministic origin-probe entry point, CRS comparison, X/Y-only
   extent-overlap check, 4-degree curve linearisation with control/generated-count
   checks, and in-traversal cancellation. Add `pcinspector_import_ogr` linking
   `GDAL::GDAL` PRIVATE.
3. Write `tests/fixtures/OgrFixtureFactory.{h,cpp}` — GeoJSON as inline text plus a
   GDAL-authored four-sublayer EPSG:3006 GPKG delivering multi-sublayer enumeration, a
   real projected CRS WKT, curve linearisation, and mixed geometry types in one
   artefact.
   > GeoJSON **cannot** carry the projected-CRS case: RFC 7946 pins it to OGC:CRS84
   > and GDAL reports WGS84 regardless (§3). That case must come from the GPKG.
   > The generator **must** call
   > `srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)`, or GDAL 3.x uses
   > authority (northing, easting) order and the fixture comes out transposed.
4. Add `pcinspector_ogr_fixture_generator` and `pcinspector_ogr_component_tests` with
   `FIXTURES_SETUP` / `FIXTURES_REQUIRED phase1_vector_files`, mirroring the PDAL lane.
5. Write `tests/component/OgrVectorImportTests.cpp`.

**Done when** loads produce correct counts for hole, curve, multi-geometry, collection,
null-geometry, Z-flattening, and limit cases; a huge single feature cancels promptly;
and two threads load the same dataset concurrently.

### Phase 3 — Document integration *(core APIs implemented; bounds/replacement audit pending)*

1. Add `SceneLayerKind`, `VectorLayer`, the vector collection, `vectorRevision_`, and
   the §6.6 API—including `addVectorLayer(data, initiallyVisible)`—to `SceneDocument`.
2. Make `removeLayer` and `setLayerVisible` dispatch on kind; add `isolateLayer` and
   `setAllLayersVisible`; share `nextLayerId_`.
3. Add `hasAnyLayer()`, `sceneBounds()`, `visibleSceneBounds()`, `layerBounds()`,
   `copyVectorLayersFrom()`. Leave `bounds()` and `empty()` point-cloud-only.
4. Rewire `MainWindow::isolateLayer` / `showAllLayers` onto the document methods.
5. Write `tests/unit/SceneDocumentVectorTests.cpp`.

**Done when** revision isolation, id sharing, kind dispatch, bounds separation,
initially-hidden layers, and copy-preserves-ids all pass.

### Phase 4 — Async loading and scheduler ownership *(single-job lifecycle implemented; shared bundle and Tasks integration pending)*

1. Add `ImportServices` and its production/test factories (§6.5). Remove
   `waitForIdle()` from `~PointCloudLoadController`; expose scheduler identity from
   both controllers; make each destructor cancel only its own work; validate the
   three-member bundle in `MainWindow`; make worker closures independent of controller
   lifetime; and implement explicit owner shutdown that destroys both controllers,
   waits once, then destroys the scheduler. Own commit.
2. Write `VectorLoadController.{h,cpp}` into `pcinspector_import_async` (§6.5) —
   single job, deterministic origin-probe gate, per-sublayer tasks,
   `fairnessGroup = jobId`, configured working-ceiling admission, partial outcomes,
   generation-safe cancellation, and failed/not-yet-emitted retry.
   > **AUTOMOC is globally OFF** (`CMakeLists.txt:80`). This target already opts in
   > (`src/CMakeLists.txt:156`); a *new* target with a `Q_OBJECT` class would need
   > `set_target_properties(... AUTOMOC ON)` or you get "undefined reference to
   > vtable" with no useful diagnostic.
3. Add pure `src/import/LoadJobKey.h` to `pcinspector_import_api` and Qt-based
   `src/import/LoadJobRow.h/.cpp` to `pcinspector_import_async`; have both
   controllers project into `LoadJobRow`.
4. Write `tests/qt/VectorLoadControllerTests.cpp` against a `FakeVectorLoader` — no
   GDAL dependency — in the existing `pcinspector_qt_async_tests`.

**Done when** the single-job progression, deterministic origin probe,
per-sublayer scheduling, all-success/partial-success/none-success resolution,
awaiting-choice cancellation, late-result discard, duplicate-free retry, bundle
rejection, single owner wait, and "both kinds reach the injected scheduler" all pass.

### Phase 5 — Render pass restructure *(implemented; native Metal depth qualification passed)*

1. Extract `FrameCamera` (including near/far planes,
   `worldUnitsPerPixelAtDepth`, and `shaderNearPlaneW`) and route `buildFramePlan`
   and `buildDrawList` through it.
2. Rename `PointCloudRenderer::draw` → `recordDraws`; split
   `EyeDomeLightingPass::composite` into `updateUniforms` + `recordComposite`. Rename,
   do not wrap. Hoist the duplicated clear colour into `sceneClearColorRgba`.
3. Add `recordScene()`, collapsing the duplicated branches at `:861-875` and
   `:882-896`. Move every `updateUniforms` above the first `beginPass`.
4. Add `gl_FragDepth = centerDepth;` to `shaders/edl.frag` immediately after sampling
   `centerDepth` and before every early return; set the EDL pipeline to
   `depthTest(true) / depthOp(Always) / depthWrite(true)`.

Sub-steps 1–3 are behaviour-preserving and worth their own commit. **Done when** the
existing renderer, contract, and GPU-lane tests pass unchanged and screenshots with
EDL on and off match the pre-refactor build.

### Phase 6 — Vector rendering *(implemented; native Metal qualification passed)*

1. Register the six shaders in `cmake/PciShaders.cmake:9-18`.
2. Write the shaders (§6.8), including the `nearPlaneW` near-plane test in both
   `vector_line.vert` and `vector_marker.vert`, with all size uniforms passed in
   render-target pixels and no device-pixel-ratio multiplication.
3. Write `VectorLayerRenderer.{h,cpp}`: `VectorLayerUniform` with its offset asserts,
   six pipelines against `renderTarget()->renderPassDescriptor()`, per-layer immutable
   buffers keyed on id **and** data pointer, 16-bit indices per fill batch, and the
   `QRhi::Instancing` capability error.
4. Add `buildVectorDrawList()`, `refreshVectorLayers()`, `seenVectorRevision_` (reset
   in `setDocument`), eight-corner conservative culling with farthest-depth inflation,
   and switch framing to `visibleSceneBounds()` / `layerBounds()`.
5. Add the three counters to `RenderMetrics`.
6. Add `VectorOverlayCapability` query/callback to `RenderViewport` and implement the
   Unknown → Supported/Unsupported transition in the RHI viewport.
7. Write `tests/qt/VectorLayerRendererTests.cpp` and
   `tests/gpu/VectorRenderGpuTests.cpp`; add the vector-only case to
   `tests/ui/RendererContractTests.cpp`.

**Done when** the GPU depth matrix in §8 passes on the Metal lane and the near-plane
cases render finite quads.

### Phase 7 — UI *(implemented; manual visual pass pending)*

1. `VectorColorButton`, non-blocking `VectorSublayerDialog`, bulk selection, and their
   tests.
2. Typed `LoadJobKey`/`LoadJobRow` Tasks-dock refactor across both controllers,
   including live failure, partial-success, cancellation-retains-layers, and retry
   row text/actions. Row actions are visible as well as available from the context
   menu.
3. The contextual vector Inspector page, readable alpha swatches, percent opacity,
   appearance reset, scene-relative placement actions, metadata, placement notice,
   reported-CRS/extent warnings plus **Show anyway**, `setVectorLayers`,
   `kLayerKindRole`, and the new callbacks.
4. `importVectorLayerAction`, the `chooseVectorLayers` flow, `copyVectorLayersFrom` in
   the Replace path at `:818-829` and `:1553`, `hasAnyLayer()` in `updateUiContext()`,
   capability-gated enablement, XY-disjoint hidden admission, independent cancel
   enablement, and two lines in `refreshLayerPanel()`.
5. Extend `tests/ui/MainWindowTests.cpp`, including direct Tasks actions,
   extent-warning projection, metadata, percent opacity, and scene-relative
   placement.

**Done when** the manual pass in §9 succeeds.

### Phase 8 — Documentation *(draft present; reconcile with completed UI before release)*

README section, a row in `UI_DESIGN_LANGUAGE.md`'s placement matrix, and an explicit
statement that v1 is a planar overlay and not terrain draping.

### Phase 9 — Release readiness *(not started; gates the version claim)*

1. **CI lane at the declared minimum GDAL (3.4)** — a prerequisite for advertising
   3.4 support, not a follow-up. Until it exists the README must say "developed and
   tested against GDAL 3.13".
2. **Runtime deployment of GDAL and PROJ.** `install(TARGETS pcinspector BUNDLE …)`
   at `CMakeLists.txt:108` means installed builds are a supported artifact, so this is
   in scope. `qt_generate_deploy_app_script` will **not** carry `libgdal`, `libproj`,
   or `proj.db`.
3. **Clean-machine smoke test**: install the bundle on a host without Homebrew, open
   the EPSG:3006 GPKG, and assert a non-empty CRS string. This is what makes a missing
   `proj.db` fail loudly instead of silently showing `—`.

---

## 8. Test plan

All Catch2, via `pci_add_catch_test_target` (`cmake/PciTesting.cmake:3`).

**`tests/unit/VectorGeometryTests.cpp`** — origin-relative conversion survives
SWEREF99 magnitudes, with `static_cast<float>(6580000.001) == static_cast<float>(6580000.0)`
in the same test to document *why* the origin exists · square → 2 triangles · square
with a hole → area excludes the hole, every index in range · **OGR-closed rings
de-duplicate before triangulation** · **consecutive duplicate coordinates are
stripped** · CW and CCW exteriors triangulate identically · fewer than 3 distinct
points → no fill, outline kept · **a bowtie fails area validation → outline only,
`unfilledPolygons == 1`** · **a ring touching itself at one vertex** · a valid ring is
not falsely rejected · NaN/infinity rejects the part and reaches no array · ring of *n*
→ *n* segments including the closing one · **`wkbLinearRing` emits its closing
segment** · open line string of *n* → *n − 1* · zero-length segments dropped · holes
stroked · **one connected polygon at 65,535 / 65,536 / above 65,536 referenced
vertices is partitioned on triangle boundaries with no lost triangle** · a vertex
referenced across a boundary is present in both batches and both copies are counted ·
bounds cover every kind · `vectorLayerOrigin` snaps stably including negatives ·
**parts counters increment inside recursion while `featureCount` does not**.

**`tests/unit/VectorGeometryLimitsTests.cpp`** — every builder-owned
input-polygon, emitted-fill, fill-index, segment, marker, retained-byte, and
application-working-byte limit throws `VectorImportLimitExceeded` naming itself ·
limits are checked *before*
application allocation · input vertices and emitted vertices are independently
observable · **a boundary-duplicated vertex alone pushes the emitted-vertex limit,
and separately the retained-byte limit, over the ceiling** · transient charges are
released · overflow-safe arithmetic near `uint64` max · a limit violation leaves the
builder unusable but not corrupt.

**`tests/unit/VectorLayerStyleTests.cpp`** — defaults differ by dominant kind ·
clamping bounds every numeric field · non-finite values fall back to defaults.

**`tests/unit/SceneDocumentVectorTests.cpp`** — adding a vector layer bumps
`vectorRevision()` and **not** `revision()` · style edits bump only `vectorRevision` ·
point edits bump only `revision` · an idempotent edit returns false and bumps nothing ·
ids never collide across kinds · `removeLayer` / `setLayerVisible` / `isolateLayer` /
`setAllLayersVisible` dispatch on kind and cover **both** collections ·
**`bounds()` ignores vector layers** · `sceneBounds()` unions both and applies
`zOffset` · `visibleSceneBounds()` excludes hidden layers of either kind · a degenerate
single-point extent is inflated · a vector-only document reports `empty() == true` and
`hasAnyLayer() == true` · null data throws `std::invalid_argument` ·
`addVectorLayer(data, false)` retains the layer while excluding it from
`visibleSceneBounds()`, and an explicit visibility change includes it ·
**`copyVectorLayersFrom` leaves the source intact, preserves ids, and advances
`nextLayerId_`** · styles are clamped on the way in.

**`tests/component/OgrVectorImportTests.cpp`** — driver and sublayer enumeration · all
four GPKG sublayers with their types · **sublayers resolve by index, including
duplicate names** · inspect never forces a count · **the EPSG:3006 WKT contains
`SWEREF99`** (this also guards against a broken PROJ data path) · Z flattened with
`sourceHadZ` · hole triangulated · curve linearised to > 8 segments ·
the 4-degree curve step is deterministic · invalid angle-step configuration is
rejected before reading · source features and curve control points are rejected
before application expansion, and generated points are validated afterward · MultiPolygon and
GeometryCollection walked with parts counted correctly · null and empty geometries
skipped as *features* not *parts* · a sublayer subset honoured in request order ·
**a pre-requested stop token throws `VectorImportCancelled`** · **a single
million-vertex feature cancels within one poll interval** · progress monotonic ·
missing file and raster-only file throw `VectorImportError` · **`crsMismatch` set for
EPSG:3006 data against a WGS84 target and clear when they match** ·
**`extentDisjointXY` compares X/Y only: non-overlapping X/Y sets it, while overlapping
X/Y with disjoint Z does not** · **origin: supplied X/Y is snapped; absent origin uses
the extent union; the origin probe visits keys/features deterministically, cancels
mid-traversal, and gives all extent-less sublayers one origin; > 200 km extent
fails** · two threads loading one dataset both succeed.

**`tests/qt/VectorLoadControllerTests.cpp`** — one job id spans inspection and load ·
extent-known phases progress `Queued → Inspecting → AwaitingChoice → Reading → Ready`
on **one** row, while extent-less adds `ResolvingOrigin` · `continueLoad` on an
unknown/terminal job or empty selection returns false · zero inspected sublayers fail
once · one task per selected sublayer, all in
`fairnessGroup = jobId` · an extent-less selection schedules one origin probe and no
load begins before it finishes · all succeed → one `Ready`
`finished(summary)` · one fails and one succeeds → nonterminal `sublayerFailed`, then
one partial-success `Ready` summary with warning detail · all fail → one job-level
`Failed`, no `finished` · cancel emits `cancelled` once and never
`sublayerLoaded`/`sublayerFailed` afterward, while already-emitted layers remain ·
cancel while `AwaitingChoice` transitions to `Cancelled` · a late success/error after
cancel only drains outstanding state and retry enables only after that drain · retry
after partial success or cancellation
submits failed/not-yet-emitted keys only and never duplicates successful emissions ·
`VectorImportCancelled` maps to `cancelled`, not `failed` · each controller destructor
cancels only its own jobs and returns without waiting · both scheduler identities
match the injected scheduler · a zero/oversized application working ceiling is
rejected before queuing · progress throttled and monotonic.

**`tests/qt/VectorLayerRendererTests.cpp`** — all pure, no device, mirroring
`PointCloudRendererTests.cpp:125-169`: `VectorLayerUniform` size and offsets ·
`stageVectorLayerUniforms` stride and byte layout · capacity growth reuses
`grownUniformDrawCapacity` · `vectorPipelineIndex(primitive, depthMode)` maps uniquely
onto six slots · **`worldUnitsPerPixelAtDepth` is constant and nonzero for
orthographic, and scales linearly with depth for perspective** ·
**`vectorLayerCullBounds` evaluates all eight corner depths** · a depth-spanning
perspective layer inflates at its farthest relevant corner · a layer wholly before
the near plane is rejected · a near-plane-spanning layer remains finite · a layer at
or beyond the far plane clamps inflation to the camera far distance and is then
frustum-tested · an anchor just outside a viewport edge remains visible when its
stroke/marker overlaps the viewport · orthographic inflation is constant and nonzero ·
style/feather sizes remain identical render-target-pixel values under mocked device
pixel ratios 1 and 2 ·
`shaderNearPlaneW` returns the near distance for perspective and 0 for orthographic ·
`buildVectorDrawList` culling and the zOffset model matrix.

**`tests/ui/VectorSublayerDialogTests.cpp`** — every sublayer listed with type and
count · unknown count renders as text, not `-1` · all checked by default · Clear
disables Add · **selection returns keys in preflight order** · **completes without
a nested event loop** · **an external cancel closes the dialog**.

**`tests/ui/MainWindowTests.cpp`** — the import action is shared between menu and
toolbar · vector import stays disabled for Unknown/Unsupported renderer capability
and enables on the Supported callback · a single-sublayer source skips the picker · a
multi-sublayer source shows it ·
an imported layer appears after the point-cloud rows · the Inspector switches to the
vector page · a stroke-width change bumps `vectorRevision` only · the checkbox hides a
vector layer · **Isolate and Show All cover vector layers** · Statistics disabled for a
vector selection · **Fit Scene frames a vector-only scene** · **replacing the
point-cloud scene keeps overlays, and a failed replace rolls back with overlays
intact** · **Cancel Loading is enabled and effective with only a vector job active** ·
**Tasks rows for the two kinds with equal numeric ids do not alias** · Prioritize is
absent on vector rows · full/partial/failed/cancelled Tasks text and retry actions
match §6.9 · **an XY-disjoint layer is retained and hidden, its warning appears, Fit
Scene bounds remain unchanged, and Show anyway makes it visible and only then changes
Fit Scene** · **the placement notice appears when Z = 0 is outside the scene Z range,
and "Place above scene" clears it** · invalid/null/mismatched `ImportServices` bundles
throw · destruction with both controllers active destroys both, waits once, and only
then releases the scheduler.

> **Never click `vectorFillColorButton` in a test.** `QColorDialog` under
> `QT_QPA_PLATFORM=offscreen` hangs. Assert the style path by driving the document and
> checking that the widgets update.

**`tests/gpu/VectorRenderGpuTests.cpp`** (opt-in lane). Screenshot equality only proves
colour, so the depth republication is validated directly — this is the most
backend-sensitive part of the feature:

| Case | EDL off | EDL on |
|---|---|---|
| Depth-tested vector **behind** a point surface | occluded | occluded |
| Depth-tested vector **in front** | visible | visible |
| **Always-on-top** vector behind a point surface | visible | visible |
| Far-plane vector over a background pixel (no points) | visible, proving background depth is 1.0 | visible, proving republished background depth is 1.0 |

plus near-plane cases: one endpoint between eye and near plane; one endpoint behind
the eye; both endpoints behind the near plane; and a grazing-angle line — each
asserting a finite, plausibly-sized quad rather than a screen-filling one, with EDL
both on and off. Also fill colour readback and one connected polygon spanning two
16-bit batches, plus two overlapping always-on-top layers proving later document
order blends last.

---

## 9. Verification

```sh
cmake --preset ci && cmake --build --preset ci --parallel && ctest --preset ci
cmake --preset native-gpu && cmake --build --preset native-gpu --parallel && ctest --preset native-gpu
```

The `ci` preset sets `PCINSPECTOR_WARNINGS_AS_ERRORS=ON` and is the real gate —
earcut and `GetExtent` will only fail there. `native-gpu` is the Metal lane with
validation on.

### Native GPU vector qualification record

Qualified on **2026-07-30** using **Apple M4 Pro / Metal** with RHI validation
enabled. The focused vector executable passed **143 assertions in 6 test cases**,
and the complete `native-gpu` CTest lane passed **28/28 tests**.

The six native vector cases cover the full matrix from §8: vector-only rendering;
depth-tested behind/in-front and always-on-top rendering with EDL off and on;
far-plane background-depth republication; all four near-plane configurations with
finite-size bounds and EDL off and on; exact polygon fill colour; a connected fill
split at the 65,536-local-vertex boundary; and deterministic painter order for
overlapping always-on-top layers.

### Performance acceptance criteria

Measured on the named reference backend **Apple M2 Pro / Metal at 1440p** with
`PCI_PROFILE_RENDERING=1` on `test_data/3445-343.laz`, each compared against a
pre-feature build:

| Case | Criterion |
|---|---|
| No vector layers | Frame time within noise; `vectorDrawCalls == 0` |
| Layers present, all hidden | Frame time within noise; `vectorDrawCalls == 0` |
| Representative layer (~50 k segments, ~20 k triangles) | ≤ 0.5 ms added GPU time |
| Largest legal generated payload under the default 256 MiB application working-byte ceiling | Frame time bounded and interactive (≥ 30 fps); `gpuVectorBytes` reported and ≤ `maximumRetainedBytes` |

Record all four in the PR description. The point budget *is* expected to yield detail
in the last case; that is the contract in §1.1, not a regression. The generator grows
a mixed fill/segment/marker payload until the next append would exceed
`maximumApplicationWorkingBytes`; it does not attempt to make unrelated limits equal
simultaneously.

### Manual pass

Use the GDAL-authored, standards-compliant **EPSG:3006 GPKG** fixture covering the test
tile's extent (334 063–334 606 E, 7 400 188–7 400 775 N), with a polygon plus hole, a
line, and points. The tile reports no SRS, so there is no target CRS to mismatch even
though the GPKG correctly reports EPSG:3006. Then:

1. Open `test_data/3445-343.laz`, import the EPSG:3006 GPKG. Fill, stroke, and markers draw.
   The placement notice appears (Z = 0 is ~750 m below); **Place above scene** clears
   it and the layer becomes visible from above.
2. Toggle Depth enhancement. Vector colours must be **identical** either way.
3. Toggle Always on top over dense terrain.
4. Orthographic and top-down views, plus a grazing angle putting part of a line
   between the eye and the near plane.
5. A multi-sublayer GPKG: picker appears, unchecking works, cancelling from the Tasks
   dock closes it.
6. Import into an **empty** scene — the camera must frame it.
7. Load a second point cloud in Replace mode; overlays survive. Cancel one; overlays
   still survive.
8. **CRS warning**: import the EPSG:3006 GPKG against a point cloud that reports a
   different SRS and confirm the reported-CRS mismatch warning.
9. **XY-extent warning**: import a layer whose X/Y extent is far from the cloud.
   Confirm it is retained but hidden, the warning and **Show anyway** action appear,
   and Fit Scene is unchanged. Invoke **Show anyway** and confirm Fit Scene then
   includes it.
10. Force one selected sublayer to fail and cancel another run after its first layer
    appears. Confirm partial-success/cancelled Tasks text, retained successful layers,
    and retry without duplicate Scene rows.

---

## 10. Risk register

| # | Risk | Mitigation |
|---|---|---|
| 1 | **earcut will not compile under `-Werror`** | `SYSTEM` include directory. Highest-probability CI break. |
| 2 | **OGR closed rings fed to earcut** give wrong-but-plausible triangulations | Strip the duplicate; dedicated regression test. |
| 3 | **Invalid/self-intersecting polygons** silently mis-triangulate — earcut does not report failure | Area-vs-shoelace validation, outline-only fallback, `unfilledPolygons` counter. The larger real-world risk than #2. |
| 4 | **A single huge feature exhausts memory or blocks cancellation** | Split incremental input/emitted/index/retained/working limits; cancellation polled inside traversal. State explicitly that GDAL's materialisation of one feature cannot be strictly capped. |
| 5 | `GetFeatureCount(TRUE)` full-scans GML/DXF/CSV | Always `FALSE`; tolerate `-1` end to end. |
| 6 | `GetExtent` is `warn_unused_result` | Check the `OGRErr`. |
| 7 | `GDALDestroyDriverManager()` races live workers | Never call it; suppress in any future ASAN lane. |
| 8 | **PROJ data path** — `exportToWkt` silently returns empty without `proj.db` | CRS component test fails loudly; Phase 9 owns deployment and the clean-machine smoke test. |
| 9 | GDAL and PDAL both link PROJ and GEOS | Same Homebrew builds today; comment beside both `find_package` calls. |
| 10 | **Declared GDAL 3.4 support is unverified** | Phase 9 CI lane gates the claim; README says 3.13 until then. |
| 11 | Widening `empty()` breaks the replace-or-add prompt at ten sites | Keep it point-only; add `hasAnyLayer()`. |
| 12 | **Replace discards the document; moving overlays breaks rollback** | `copyVectorLayersFrom`, plus a failed-replace regression test. |
| 13 | **`visibleBounds()` is null for a vector-only scene** → black viewport | Switch framing to `visibleSceneBounds()`. |
| 14 | Degenerate single-point bounds → zero camera distance | Inflate zero-extent axes by ±0.5 m. |
| 15 | **Revisions are document-local**, so a new document can alias the old one's | `setDocument` resets `seenVectorRevision_`; buffers key on data pointer as well as id. |
| 16 | `pointCount_ == 0` early-outs may skip the frame for a vector-only scene | Audit the frame path in Phase 6. |
| 17 | **The eye-plane test is not a near-plane test** | `nearPlaneW` uniform; four GPU near-plane cases. Verified bug (§3). |
| 18 | **`worldUnitsPerPixel` was zero for orthographic** | `worldUnitsPerPixelAtDepth(depth)`; tested in both projections. |
| 19 | **Index width above 65,536 was undefined** | Partition the triangle stream with original-to-local remaps; always 16-bit; connected-polygon and duplication-limit tests. |
| 20 | **Missing instancing would silently draw nothing** | `VectorOverlayCapability` remains Unknown/Unsupported until probed; callback-gated import action and unsupported badge. |
| 21 | **Always-on-top layers overlap in painter order** | Deterministic: depth-tested group first, then always-on-top layers in document order; later translucent layers blend over earlier ones. |
| 22 | **Alpha double-blend at joins and overlaps** — translucent strokes darken at every vertex | Accept and document. Full fix is an offscreen RGBA8 per translucent layer. Stencil dedup is unavailable (no mid-pass clear). |
| 23 | **Picking disagrees with what is drawn** — clicking a filled roof returns the ground point behind it | Renderer-correct but surprising. Out of scope; note in the release note. |
| 24 | **Pixel-unit drift** could make vector sizes diverge from existing point sizes | All sizes are device/render-target pixels; apply no device-pixel-ratio scaling to points, strokes, markers, or feathering. |
| 25 | Resource updates inside a render pass are illegal | Structural: the EDL split exists for this. Keep the ordering visible in one place. |
| 26 | `renderTarget()` and `pointRenderTarget()` sizes must agree | They do (`:628-644`); assert rather than assume. |
| 27 | The composite writes depth even with zero vector layers; `gl_FragDepth` + `discard` disables early-Z | Negligible — one fullscreen triangle plus thin quads. Do not add a second pipeline. |
| 28 | **MSAA is a future trap** | Raising `setSampleCount` needs it on all six pipelines or `create()` fails. 1× everywhere today. |
| 29 | `VectorLayerDraw::buffers` is borrowed into the layer map | Sweep at the top of `render()`; the map must not rehash mid-frame. |
| 30 | **Shared scheduler + `waitForIdle()` in a controller destructor** can block on the other controller's work | Validated `ImportServices`; controller destructors cancel only their jobs; `MainWindow` destroys both, waits once, then destroys the scheduler. |
| 31 | **Bare `jobId` aliases across two controllers** in the Tasks dock | Typed `LoadJobKey` everywhere. |
| 32 | **Cancel Loading gated on `loading_`** ignores vector jobs | Enablement and cancellation consider both controllers. |
| 33 | Shapefile sidecar failures are opaque | Surface GDAL's own message through `OgrErrorScope`. |
| 34 | `saveState(1)` / `restoreState(state, 1)` | This design adds no dock, so no version bump — but adding one later would corrupt saved workspaces. |
| 35 | Vector items missing `kCountRole` render a blank badge | Always set it. |
| 36 | GDAL 3.x axis order transposes fixture data | `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)` in the generator. |
| 37 | Concurrent extent-less sublayers choose different origins | One deterministic, cancellable origin-probe task completes before any sublayer task. |
| 38 | A partial-success outcome is mistaken for total success or failure | Nonterminal `sublayerFailed`, ordered aggregate state, one `VectorLoadSummary` normal completion, and explicit none-succeeded failure. |
| 39 | A task completes after cancellation and emits a duplicate/late layer | Cancellation flag plus attempt generation discards all late results; retry excludes successful keys. |
| 40 | An XY-disjoint layer destroys Fit Scene on import | Retain it hidden; only explicit visibility/Show anyway admits it to `visibleSceneBounds()`. |

---

## 11. Out of scope

**Terrain draping** (per-vertex elevation sampling so a layer follows the surface) —
the significant one, and the reason §1 says "planar overlay" throughout.
Attribute-driven (categorized/graduated) styling · text labels · feature picking and
attribute tables · CRS **reprojection** (mismatch *detection* is in scope) · vector LOD
or tiling · per-feature culling · editing and export · session persistence of overlays
(`restoreWorkspace` stores only geometry and dock state, so an imported layer will not
come back — users will expect it to) · Triangle and Cross marker shapes · GEOS-based
polygon repair.

Bundling GDAL and PROJ is **no longer out of scope** — it moved into Phase 9, because
`install(TARGETS pcinspector BUNDLE …)` makes installed builds a supported artifact.

---

## 12. Decision-completeness audit

The APIs (§6), implementation phases (§7), tests (§8), verification (§9), and risks
(§10) were re-read together after the final revision:

| Contract | Public surface / fallback | Implementation | Verification |
|---|---|---|---|
| Lossless universal 16-bit fill indices | `VectorFillBatch`, split input/emitted/index/retained/working limits; outline-only validation fallback | Phases 1–2 | Connected >65,536 polygon, boundary duplication, every limit, hostile-feature component tests |
| Deterministic shared origin | `VectorLoader::probeOrigin` and one `OriginProbe` gate | Phases 2 and 4 | Extent union, deterministic extent-less traversal, cancellation, shared-origin tests |
| Partial success without duplicate retry | `sublayerFailed`, `VectorLoadSummary`, aggregate job state, generation discard | Phases 4 and 7 | All/some/none success, cancellation retention, late result, retry, Tasks text tests |
| One safe import pool | `ImportServices`, both `schedulerIdentity()` methods, explicit `MainWindow` destruction | Phase 4 | Invalid bundle, both kinds in flight, per-controller cancellation, single owner wait tests |
| XY-disjoint safety | `extentDisjointXY`, `addVectorLayer(data, initiallyVisible)`, **Show anyway** | Phases 2, 3, and 7 | X/Y-only comparison and Fit Scene stable-until-shown tests |
| Conservative planar-overlay rendering | eight-corner culling, near/far camera range, device-pixel size contract | Phases 5–6 | Depth-spanning, near-plane, far-layer, viewport-edge, orthographic, and pixel-unit tests |
| Honest backend availability | `VectorOverlayCapability` query/callback; disabled import fallback | Phases 6–7 | Unknown/Unsupported/Supported UI and renderer-contract tests |
| EDL depth compatibility | depth-republishing composite with defined `gl_FragDepth` placement | Phase 5 | EDL on/off GPU depth matrix and background-depth test |

The audit found no unresolved implementation choices. Terminology is fixed throughout:
**planar overlay**, **XY-disjoint extent**, **CRS reported by GDAL**,
**input polygon vertices** versus **emitted fill vertices**, and **partial success**.
The specification is ready to implement.
