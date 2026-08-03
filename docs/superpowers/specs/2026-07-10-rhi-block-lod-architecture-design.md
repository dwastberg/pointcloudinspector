# Rendering Architecture Redesign: QRhi Block/LOD Rebuild

Date: 2026-07-10
Status: Approved design, pending implementation planning

## 1. Goals and constraints

- **Cross-platform rendering**: the viewer must be able to add Vulkan and possibly
  Direct3D backends later without rewriting the renderer.
- **Huge point clouds are the most critical feature**: the architecture must be designed
  for billions of points (out-of-core), implemented in phases — in-memory block rendering
  first, hierarchical LOD and disk streaming after.
- Existing behavior (navigation, picking, color maps, loading progress) must survive each
  phase; the existing test suite keeps passing throughout.

## 2. Analysis of the current design

### 2.1 The renderer is already portable; its naming is not

Everything in `src/renderer/metal/` is written against QRhi, Qt's rendering hardware
interface, which ships Metal, Vulkan, D3D11, D3D12, and OpenGL backends. Shaders are
Vulkan-flavored GLSL cross-compiled by qsb (`qt_add_shaders`) into MSL/HLSL/SPIR-V at
build time. The genuinely Metal-specific code is three lines: `setApi(Api::Metal)`, the
`rhi()->backend() != QRhi::Metal` assertion, and `backendName()` returning "Metal".

Consequences:

- The `Metal*` class names, the `pcinspector_renderer_metal` CMake target, and the
  `if(APPLE)` gate in `CMakeLists.txt` misrepresent the code as platform-specific and
  invite an unnecessary per-API backend abstraction.
- **Decision: do not build a per-API abstraction.** QRhi is the portability layer. A
  native escape hatch is preserved by keeping the scene layer free of QRhi types.

### 2.2 The actual blockers to huge clouds are in the data architecture

1. **Global 16-bit quantization** (`PdalPointCloudLoader.cpp`, `points.vert`): the whole
   cloud is quantized into 65,535 steps across its maximum extent. A 10 km tile gets
   ~15 cm precision; points visibly snap to a grid.
2. **Stride decimation at import** (default cap 10M points): large clouds are uniformly
   subsampled once at load time; there is no path to viewing the full data.
3. **Monolithic vertex buffer**: QRhi buffer sizes and draw counts are `quint32`, capping
   a single buffer at ~268M points (4 GB / 16 B per point); any cloud change is a full
   destroy-and-reupload.
4. **Single-frame upload stall**: `MetalUploadManager::replacePointBuffer` submits every
   chunk within one frame's command buffer; progress callbacks are QTimer-deferred, so
   the UI freezes for the whole upload and the progress overlay jumps.
5. **First-N draw budget over a lexicographically sorted buffer**: `AdaptivePointBudget`
   draws the first N points of a buffer sorted by quantized (x, y, z), which renders a
   spatial slab rather than a representative subset. No frustum culling, no LOD.
6. Minor: the picker allocates a full-render-target-size R32UI texture and redraws the
   whole budgeted cloud to read back 5×5 pixels; `RenderViewport::setPointCloud` bakes in
   atomic whole-cloud replacement, which cannot express streaming; QRhiWidget composites
   through the widget backing store (a QRhiWindow owns a real swapchain).

### 2.3 QRhi capability ceiling (verified against Qt 6.8.3 `qrhi.h`)

**Decision: QRhi is sufficient because LOD is the scaling strategy.** Brute-force is the
only path QRhi cannot take, and it is not the chosen path. The full rationale, the taxes
QRhi still charges, and the conditions under which this decision must be revisited are
in Appendix A.

In brief: QRhi has compute pipelines but no indirect draws, no mesh shaders, and no
64-bit atomics, which rules out GPU-driven brute-force point rendering
(compute-rasterization of billions of points). The proven architecture for huge clouds
(Potree, COPC viewers) is hierarchical LOD, where any single frame draws only ~10–50M
points selected by screen-space error — comfortably within QRhi's vertex pipeline. QRhi
also provides a supported native escape hatch
(`QRhiCommandBuffer::beginExternal()/endExternal()`, `QRhiBuffer::nativeBuffer()`,
`QRhiTexture::nativeTexture()/createFrom()`), so if brute-force rendering is ever
needed, it can be added as a bounded native module inside the QRhi frame rather than by
replacing QRhi.

## 3. Target architecture

Three layers, dependency-ordered, each independently testable.

### 3.1 `pci::scene` — data model (pure C++, no Qt or GPU types)

- **`PointBlock`**: up to ~1M points in the existing 16-byte `GpuPoint` layout, quantized
  **locally**: each block stores a double-precision `origin` (3 components) and `scale`.
  A ~100 m block yields ~1.5 mm precision, fixing the global precision collapse. Each
  block carries a double-precision AABB, a point count, per-block scalar min/max
  statistics (for color normalization), and a residency state
  (`Missing | Loading | CpuResident | GpuResident`).
- **`PointCloudScene`**: owns blocks. Phase 1: a flat list of leaf blocks. Phase 2 adds
  an additive octree (COPC-style: every node holds points; children add detail), where
  each octree node owns one block.
- **`BlockProvider` interface**: how block payloads materialize.
  - Phase 1: `InMemoryProvider` (import produces all blocks up front).
  - Phase 3: `CopcProvider` (HTTP/file range reads of COPC nodes) and an
    octree-build provider with a disk cache for non-COPC sources.
  - This interface is the seam that makes out-of-core billions possible without another
    redesign: the renderer only ever asks for blocks; it never knows where they come from.

### 3.2 `pci::render` — QRhi-only, no QWidget

A rename and rework of the current `src/renderer/metal/` code:

- `MetalPointCloudRenderer` → `PointCloudRenderer`: pipeline, shader bindings, per-frame
  camera uniform, draw loop over visible blocks.
- `MetalUploadManager` → `UploadScheduler` + `BlockBufferPool`: one GPU buffer per block
  (pooled), a **per-frame upload budget** (default 48 MB per frame, tunable) so loading
  never stalls a frame and rendering is progressive, and eviction hooks for the
  out-of-core phase.
- **Culling**: per-frame CPU frustum culling over block AABBs; one draw call per visible
  block. At ≤1M points per block this is hundreds to thousands of AABB tests per frame —
  negligible CPU cost, no GPU-driven culling required.
- **Per-block transforms**: the world transform of each block is composed with the camera
  relative-to-eye in double precision on the CPU, then converted to float for the
  uniform (dynamic uniform buffer with offsets). This removes float jitter for
  georeferenced coordinates.
- `MetalPointPicker` → `PointPicker`: a small fixed offscreen target (e.g. 64×64 pixels
  centered on the cursor) replaces the full-resolution R32UI texture; only blocks whose
  bounds pass the pick test are drawn. Pick IDs become (block index, point index).
- **Backend selection policy** in one place: Metal on macOS, Vulkan on Linux, D3D12 (with
  Vulkan fallback) on Windows; overridable via `AppOptions` for testing.
  `backendName()` is derived from `rhi()->backendName()` instead of a hardcoded string.
- **Shaders**: vertex decode becomes `world = origin + local * scale` using the per-block
  uniform; the existing color-map logic is kept. Shader integer constants and C++ enums
  are generated from or checked against a single shared header to remove the manual
  coupling.

### 3.3 `pci::ui` — Qt integration

- `MetalRenderViewport` → `RenderViewportWidget`, still a QRhiWidget subclass (a
  QRhiWindow + `createWindowContainer` swapchain path is a later optimization, only if
  profiling shows compositor overhead). It keeps input handling, navigation camera,
  metrics/failure/progress callbacks.
- The `RenderViewport` contract evolves from atomic `setPointCloud(cloud)` to
  scene-based: the viewport is given a `PointCloudScene` and reacts to blocks becoming
  resident (progressive display). Load progress is derived from residency counts rather
  than a single upload loop.
- **Import**: `PdalPointCloudLoader` partitions streamed points into blocks using a
  regular grid over the source bounds (cell size chosen so a typical cell holds well
  under 1M points; overfull cells split once), instead of sorting and flattening. The
  lexicographic sort is dropped. Blocks are emitted progressively so the viewer shows
  data while the file is still being read. A configurable point cap remains until LOD
  and out-of-core land, with the default raised well above 10M.

### 3.4 Data flow (Phase 1)

```
PDAL stream ──▶ block partitioner ──▶ PointCloudScene (blocks, CpuResident)
                                            │ residency notifications
                                            ▼
RenderViewportWidget ──▶ UploadScheduler (≤ budget/frame) ──▶ BlockBufferPool (GpuResident)
        │ per frame                                                   │
        ▼                                                             ▼
  frustum cull block AABBs ──▶ PointCloudRenderer.draw(visible blocks)
```

### 3.5 Error handling

- The existing fail-fast model is kept: QRhi resource-creation failures throw, are caught
  at the viewport boundary, and surface through the failure callback.
- New failure modes: a block that fails to upload is marked failed and skipped (the scene
  keeps rendering); import partitioning errors surface through the existing
  `PointCloudImportError` path.
- Threading contract stays: import on a worker thread (Qt Concurrent), all QRhi work on
  the render thread; scene residency state transitions are the only cross-thread surface
  and are documented and mutex-guarded.

## 4. Phasing

- **Phase 0 — de-Metal-ify (mechanical, zero behavior change)**: rename `metal/`
  classes and CMake targets to backend-neutral names, add the backend selection policy,
  un-gate the app build from `if(APPLE)` (Metal-specific GPU tests stay gated), rename
  `MetalCameraUniform` → `CameraUniform`. All existing tests pass unchanged.
- **Phase 1 — blocked storage end-to-end**: block partitioning at import, per-block
  quantization and shader decode, per-block draws with frustum culling, `UploadScheduler`
  with a per-frame budget and progressive display. Resolves issues 1, 3, 4, and 5 of
  §2.2 for in-memory scales (~100–500M points); issue 2 is mitigated by the raised cap
  and fully resolved by Phases 2–3.
- **Phase 2 — octree LOD**: COPC hierarchy passthrough (the loader currently flattens it
  away), octree build for LAS/LAZ, screen-space-error node selection under a point
  budget. Replaces `AdaptivePointBudget`'s first-N semantics.
- **Phase 3 — out-of-core streaming**: `BlockProvider` disk loading and eviction, COPC
  range reads, disk cache for built octrees. Billions of points.
- **Phase 4 (only if a trigger in Appendix A.5 fires)**: QRhiWindow swapchain path;
  native compute escape hatch via `beginExternal()` interop (bounded, because the scene
  layer has no QRhi dependency).

Folded in from the 2026-07-09 codebase review: per-block scalar min/max aggregated to
per-cloud statistics for color normalization, replacing the hardcoded 0–65535 range in
the camera uniform.

## 5. Testing

- **Scene layer** (no GPU, no Qt): unit tests for block quantization round-trip
  precision, partitioning, frustum culling, residency transitions, upload-budget
  scheduling decisions.
- **Render layer**: existing renderer contract and GPU test split continues
  (`tests/qt/`, `tests/gpu/`, gated Metal smoke tests). Backend policy unit-tested.
- **Benchmarks**: a synthetic block generator (procedural, no disk) measuring frame time
  versus block count; acceptance: a ~300M-point synthetic scene renders interactively
  with culling after Phase 1; a billion-point COPC streams after Phase 3.
- **Manual verification**: load a real LAS fixture; confirm progressive display during
  load and no UI stall; confirm identical navigation/picking/color behavior after
  Phase 0.

## 6. Explicitly rejected alternatives

- **Native Metal/Vulkan/D3D12 backends behind a custom abstraction**: highest capability
  ceiling (indirect draws, mesh shaders, compute splatting) but three codepaths and
  months of effort, needed only if LOD were rejected as the scaling strategy.
- **Third-party middleware (bgfx, Diligent, wgpu)**: one codepath and more features than
  QRhi, but awkward QWidget integration, a heavyweight dependency, and still a full
  render-layer rewrite for a capability gap that LOD makes irrelevant. wgpu/WebGPU in
  particular does not even close the gap: it also lacks 64-bit atomics and mesh shaders.

## Appendix A: Why QRhi is sufficient, and when to revisit that decision

### A.1 What "brute force" means and why QRhi cannot do it

The state of the art for rendering huge point clouds *without* LOD is compute-shader
rasterization (Schütz et al.): skip the vertex pipeline entirely, have compute threads
project every point and `atomicMin` a 64-bit value packing `(depth << 32) | color` into
a screen-sized buffer, then resolve in a second pass. On desktop GPUs this renders on
the order of billions of points per frame — roughly 10–50× the throughput of
`GL_POINTS`-style rendering — because it eliminates per-vertex pipeline overhead and
rasterizer serialization on 1-pixel primitives. The related family (GPU-driven
pipelines) uses compute passes to cull against frustum and Hi-Z occlusion buffers,
writing draw commands the GPU consumes via indirect draws, with no CPU round-trip.

QRhi blocks both paths at specific points (verified against Qt 6.8.3 `qrhi.h`): no
64-bit atomics in its shader feature set, no indirect draw on `QRhiCommandBuffer` (only
`draw`/`drawIndexed` with CPU-supplied counts), no mesh shaders. It does have compute
pipelines, but compute-culling without indirect draws forces a GPU→CPU readback of cull
results — a sync stall that defeats the purpose.

### A.2 Why LOD makes the remaining workload fit QRhi

With screen-space-error-driven octree selection, points drawn per frame are bounded by
the budget, not the dataset: whether the file holds 100M or 20B points, a tuned budget
draws ~10–50M points per frame. At 16 bytes per vertex with a trivial vertex shader and
~1–2 billion points/sec of vertex throughput on current hardware, that is ~2–5 ms of GPU
time. Draw-call count is tens of calls (one per visible block), so CPU submission
overhead through QRhi's abstraction — its one structural inefficiency — is irrelevant.
Frustum-culling a few thousand block AABBs on the CPU costs microseconds.

The strongest evidence is empirical: Potree renders multi-billion-point datasets in
WebGL, an API with no compute, no indirect draws, and no storage buffers — strictly
weaker than QRhi. LOD alone carries it. Once LOD is the strategy, the bottlenecks move
to disk/network I/O, LAZ decode, upload bandwidth, and node-selection quality. None of
those are graphics-API problems.

### A.3 The taxes QRhi still charges even with LOD

- **Upload path**: `uploadStaticBuffer` copies data into a staging batch on the render
  thread; there is no persistent mapping and no async transfer queue. At the planned
  48 MB/frame budget this is roughly 1–2 ms of memcpy per frame during loading —
  acceptable and tunable, but a real tax native Vulkan/Metal would not charge.
- **Occlusion culling**: without indirect draws, GPU occlusion culling is impractical,
  so culling is limited to CPU frustum tests plus LOD. For aerial/terrain clouds this
  barely matters; for dense multi-room indoor scans it gives up real performance.

### A.4 What LOD itself costs (when brute force would genuinely be wanted)

LOD needs preprocessing (octree construction for non-COPC input — minutes for billions
of points), shows pop-in during refinement, and renders an approximation during
navigation. Compute rasterization renders every point, exactly, with zero
preprocessing: drop a raw 800M-point LAS and see all of it immediately. If the
product's core workflow ever becomes "QC raw scans at full detail with no import wait",
QRhi's ceiling becomes the product's ceiling and the escape hatch (A.5) is required.

### A.5 Replacing QRhi is a false dichotomy; use the escape hatch instead

QRhi has a supported native interop surface: `beginExternal()/endExternal()` records raw
Metal/Vulkan commands inside a QRhi frame, and
`nativeBuffer()`/`nativeTexture()`/`createFrom()` pass resources across the boundary in
both directions. If brute-force rendering or GPU-driven culling is ever needed, the
solution is a bounded native module per backend (Metal first) that reads point buffers
QRhi owns, rasterizes into a texture, and returns it for QRhi to composite — while UI,
picking, overlays, shader baking, window integration, and the second backend's
remaining 95% stay on QRhi. The scene layer is kept free of QRhi types precisely so
this module stays bounded (Phase 4). Wholesale replacement would buy the same features
plus a new shader toolchain (losing qsb), hand-rolled swapchain/resize/DPI management
inside Qt widgets (losing QRhiWidget), and a full render-layer rewrite.

**Triggers to revisit this decision** — any one of:

1. Phase 1/2 profiling shows the vertex-pipeline point draw is the frame bottleneck at
   target budgets on target hardware (e.g., cannot hold 60 fps at ~30M points/frame on
   an Apple M-series GPU).
2. The product commits to full-detail, no-preprocessing viewing of >200M-point files as
   a core workflow.
3. Indoor/occlusion-heavy datasets become primary and CPU frustum culling plus LOD
   demonstrably over-draws.

If a trigger fires, the answer is the `beginExternal()` hybrid, not replacement.
Optionally, a time-boxed spike after Phase 1 — a native Metal compute rasterizer for one
resident block, composited via the escape hatch — can measure the real headroom on
Apple Silicon in a few days' effort without touching the architecture.
