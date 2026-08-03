# Render Pipeline Review and Improvement Plan

Status: archived 2026-07-31; this completed 2026-07-24 implementation plan is superseded by the current code and the Point Cloud Inspector Modernization Refactoring Guide.

## Purpose

This document reviews the point-cloud render path from imported points to GPU
submission and picking. It turns the review findings into an implementation
plan: every item identifies the files to change, the proposed design, the tests
to add, and a concrete completion criterion.

## Completion audit (2026-07-24)

All repository-controlled implementation work in items 1–12 is complete for
the documented local-file scope. The implementation includes bounded shared
CPU/GPU residency, persistent local LAS/LAZ paging, centralized scheduling,
globally fair planning, progressive publication, event-driven rendering,
candidate-narrowed picking, portable backend selection, instrumentation,
qualification tools, and deterministic/native-GPU regression coverage.

The remaining gates called out below are deliberately external qualification,
not missing source code:

- measure a materially different production dataset before claiming support
  for that dataset and allocator/I/O environment;
- qualify remote HTTP/EPT traffic before making remote deployment claims; and
- operate additional validation-enabled GPU CI runners before promoting those
  backends from opt-in to required.

Subsequent visual work is also implemented: independently authored,
non-GPL eye-dome lighting is enabled by default with a main-toolbar checkbox,
and hardware point size is selectable from 1–8 pixels through the same toolbar.
Native-GPU image tests verify both effects and their safe runtime toggling.
Circular splats, density-aware sizing, and MSAA remain measured future options,
not incomplete requirements of this plan.

This audit was validated with the warnings-as-errors CI configuration: all 193
registered non-GPU tests passed, including component and >10× residency stress
workloads. Release and profiling configurations also build cleanly. The
validation-enabled native Metal selection passed all 19 CTest entries: 14
focused GPU cases, four renderer smoke cases, and their fixture generation.

The current pipeline has a good base:

- `GpuPoint` is a compact 16-byte vertex.
- Block-local 16-bit quantization reduces bandwidth while the camera-relative
  model transform protects rendering precision.
- CPU frustum culling and a frame-based point budget already exist.
- GPU uploads are throttled.
- Picking uses a small integer render target and asynchronous readback.
- QRhi resources are separated into renderer, upload, and picking classes.

The original scalability problem was that the point budget limited drawing but
not residency. Items 2 and 12 now solve that ownership problem with independent
decoded-CPU and GPU byte budgets backed by reloadable sources. Ordinary local
LAS/LAZ above one million points is converted once to the persistent local page
store and then shares the same bounded cache, scheduler, hierarchy, and planner
contracts as COPC/EPT. Only the explicitly small-source compatibility path
retains capped flat blocks in memory, with its allocation reserved against the
same document budget.

The original per-draw uniform growth defect is also fixed. The picker consumes
the renderer's current layout-compatible binding set, and native-GPU
regressions exercise growth past both 256 and 512 simultaneously drawn blocks.

## Responses to review comments

The following decisions explicitly address the six concerns raised during
review.

1. **CPU memory must be bounded too — agreed.** GPU-only eviction is an
   intermediate improvement, not out-of-core rendering. The final residency
   design in item 2 now has separate decoded-CPU and GPU caches, byte budgets,
   pinning, and eviction. The scene retains lightweight source and hierarchy
   metadata rather than every decoded `GpuPoint` vector. Acceptance tests bound
   both caches and total steady-state process growth.

2. **COPC/EPT must be evaluated first — agreed.** Item 2 now starts with a
   mandatory architecture spike and decision record. COPC is the preferred
   first candidate because it packages range-readable LAZ chunks in an octree;
   EPT remains important for existing deployments and remote datasets. A
   bespoke hierarchy is permitted only if measured requirements cannot be met
   through PDAL queries or a focused COPC/EPT adapter. PDAL support is valuable,
   but does not remove the need to define stable node IDs, cancellation,
   scheduling, CPU/GPU caches, and pinning in the application.

3. **The picker fix was over-designed — agreed.** Item 1 no longer introduces
   resource generations or rebuilds the picker on every uniform-buffer growth.
   The picker will not cache the renderer's resource bindings. Its `record()`
   call receives the renderer's current layout-compatible binding set.

4. **Severity and user-value ordering needed correction — agreed.** Item 1 is
   now described as a bounded correctness edge case. Event-driven rendering
   and progressive publication are the highest user-impact fixes because they
   affect ordinary sessions. The implementation order still includes the cheap
   binding fix early, without presenting it as the dominant runtime risk.

5. **The program needs shippable boundaries — agreed.** Release 1 is now a
   self-contained deliverable with no dependency on the hierarchy migration.
   Hierarchical/out-of-core rendering starts with its own design decision and
   can ship separately. Required CI initially covers one primary backend and
   one portable fallback on runners the project actually owns; other backends
   remain opt-in until reliable runners exist.

6. **Large-coordinate precision is correctness work — agreed.** The
   block-local affine normalization is separated from colour-map optimization
   and moved into Release 1. UTM-scale data is a primary PDAL use case, so
   visibly quantized coordinate gradients should not wait for visual polish.

## Current data flow

Sources now diverge only while establishing their reloadable page contract:

1. Small local LAS/LAZ uses the explicitly capped retained-flat compatibility
   path. Larger ordinary LAS/LAZ builds or reopens a persistent
   `LocalPointPageSource` under the application cache directory.
2. COPC/EPT creates a `PdalHierarchicalPointSource`. Both hierarchical source
   families publish a coarse root and retain later decoded payloads in the one
   document-owned, byte-bounded `DecodedPageCache`.
3. `RenderSelection` applies frustum visibility, projected error, hysteresis,
   request limits, and the current point budget to stable logical node IDs.
4. `PointCloudScene` performs cancellable node queries through a document-wide
   FIFO decode limit. The document divides one CPU budget fairly between
   visible layer LRUs; hidden layers retain only their coarse-root allowance.
5. `UploadScheduler` uploads selected/prefetched blocks into a stable-key,
   byte-bounded global GPU LRU. CPU payload ownership is released after QRhi
   staging has copied the data.
6. `PointCloudRenderer` uploads one uniform record per draw and renders points.
7. When requested, `PointPicker` receives the renderer's current
   layout-compatible bindings, repeats the selected geometry pass into an
   `R32UI` target, and asynchronously reads a 5x5 region.

## 1. Stop the picker from caching replaceable renderer bindings

**Implementation status (2026-07-15): complete foundation.** The picker no
longer caches renderer bindings, recording receives the current binding set,
layout compatibility is checked, and 257/513-draw GPU regression coverage is
present. Renderer binding replacement is transactional: new buffer and SRB
objects are created successfully before the old pair is released. Running the
GPU regression with validation on a required CI runner remains part of the
item 10 infrastructure work, not an open design change in this item.

**Priority:** Release 1 correctness guard; low-frequency current exposure and
low implementation cost.

### Problem

`PointCloudRenderer` starts with uniform capacity for 256 draws. When a frame
needs more, `ensureUniformCapacity()` deletes and replaces the uniform buffer
and `QRhiShaderResourceBindings`. The `PointPicker` still holds the old
bindings pointer because `QRhiWidget::initialize()` is not called for every
frame. A pick after the resize can therefore use a stale QRhi resource.

This is a real use-after-replacement bug, but it is presently a bounded edge
case: it requires more than 256 simultaneously drawn blocks and a subsequent
pick. The shader-resource layout is invariant—one dynamic uniform-buffer
binding—even when the buffer object changes. QRhi permits a binding set that is
layout-compatible with the pipeline's original set, so the picker does not
need to cache the renderer-owned set or track generations.

### Files to edit

- `src/renderer/rhi/PointCloudRenderer.h`
- `src/renderer/rhi/PointCloudRenderer.cpp`
- `src/renderer/rhi/PointPicker.h`
- `src/renderer/rhi/PointPicker.cpp`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `tests/gpu/PointPickerGpuTests.cpp`

### Implementation instructions

1. Remove the renderer-owned `QRhiShaderResourceBindings *` member from
   `PointPicker`. The picker must not retain a pointer to a replaceable binding
   set between frames.

2. Change the recording API to receive the current renderer bindings, for
   example:

   ```cpp
   void record(QRhiCommandBuffer &commandBuffer,
               QRhiShaderResourceBindings &currentBindings,
               /* existing pick arguments */);
   ```

3. Let `PointPicker::ensureResources()` use the renderer's current binding set
   when it initially creates the graphics pipeline, but do not store that
   pointer. A later layout-compatible binding set can be supplied to
   `setShaderResources()` while recording.

4. Store only `serializedLayoutDescription()` in debug builds, or keep a
   picker-owned layout description, and assert that the binding set supplied to
   `record()` has the expected invariant layout. Do not introduce a renderer
   resource-generation API for this invariant-layout case.

5. In `RenderViewportWidget::render()`, update uniforms first, then pass
   `renderer.shaderBindings()` directly to the picker in the same frame. The
   getter must always return the current binding set after any capacity growth.

6. Keep renderer replacement ordering internally safe: create the new uniform
   buffer and binding set successfully, switch the renderer to them, then
   release the old objects. The picker needs no special rebuild because it no
   longer owns or retains either pointer.

### Tests

- Add a GPU regression test containing at least 257 simultaneously visible
  blocks.
- Wait until all test blocks are resident, request a center pick, and verify
  that it completes with the expected ID and no QRhi validation failure.
- Repeat the pick after another capacity growth, such as 513 visible blocks.

### Acceptance criteria

- Picking remains correct after every uniform-capacity expansion.
- QRhi validation layers report no stale or destroyed resource use.
- No generation counter or picker rebuild is required for uniform-buffer
  capacity changes while the binding layout remains invariant.
- Capacity expansion is covered by a test; existing tests currently exercise
  only one or two blocks.

## 2. Decide the hierarchical source strategy, then bound CPU and GPU residency

**Implementation status (updated 2026-07-24): complete for repository-owned
local-file behavior; remote qualification remains external.** The COPC/EPT
query-adapter architecture,
shared decoded-CPU budget, bounded GPU LRU, globally admitted cancellable scene
workers, screen-space-error selection, hysteresis, coarse-coverage transition,
runtime budgets, tests, and residency metrics are implemented. The decision
and cache contracts are recorded in
[`HIERARCHICAL_POINT_SOURCE_DESIGN.md`](HIERARCHICAL_POINT_SOURCE_DESIGN.md).
Large ordinary local LAS/LAZ now uses the persistent page-source implementation
from item 12; only small sources retain the documented, budget-admitted
in-memory fallback. Representative remote HTTP/EPT traffic measurements remain
a deployment-qualification task; the default hermetic suite does not depend on
an external endpoint.

This status means a functional MVP with global application residency controls,
not production-qualified out-of-core behavior. Before making that production
claim, complete the representative remote, churn, and peak-RSS qualification
below and measure the explicitly bounded decoder/root/lease allowances.

### Original problem

`UploadScheduler` eventually uploads every block in visible layers, and
`retainReferencedBlocks()` keeps buffers for hidden layers as long as their
CPU blocks remain in the document. `AdaptivePointBudget` limits draw calls but
does not limit memory. More importantly, the scene currently owns all decoded
`PointBlock` objects, so adding GPU LRU eviction alone still leaves process
memory growing linearly with loaded point count.

For 100 million points, the 16-byte vertex data consumes approximately 1.6 GB
on the GPU and another 1.6 GB in the retained CPU `GpuPoint` vectors, before
CPU attributes and allocator overhead. The current approach also spends the
draw budget on complete near blocks, which can leave distant parts of the cloud
empty instead of showing a coarse representation.

This is not merely a renderer data-structure task. The application needs a
reloadable, spatially queryable source behind two bounded caches. COPC and EPT
already provide hierarchical, spatially queryable point-cloud storage and PDAL
has readers for both. That option must be measured before designing a custom
octree or permanent cache format.

### Files to edit or add

- Add `HIERARCHICAL_POINT_SOURCE_DESIGN.md` after the decision spike.
- `src/import/pdal/PdalPointCloudLoader.h`
- `src/import/pdal/PdalPointCloudLoader.cpp`
- Add `src/import/PointCloudDataSource.h`
- Add `src/import/pdal/PdalHierarchicalPointSource.h`
- Add `src/import/pdal/PdalHierarchicalPointSource.cpp`
- Add `src/scene/PointCloudNode.h`
- Add `src/scene/PointCloudNode.cpp`
- Add `src/scene/DecodedBlockCache.h`
- Add `src/scene/DecodedBlockCache.cpp`
- `src/scene/PointCloudScene.h`
- `src/scene/PointCloudScene.cpp`
- `src/renderer/rhi/UploadScheduler.h`
- `src/renderer/rhi/UploadScheduler.cpp`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- Add `src/renderer/RenderSelection.h`
- Add `src/renderer/RenderSelection.cpp`
- `src/CMakeLists.txt`
- `tests/CMakeLists.txt`
- Add `tests/component/HierarchicalPointSourceTests.cpp`
- Add `tests/unit/DecodedBlockCacheTests.cpp`
- Add `tests/unit/RenderSelectionTests.cpp`
- Extend `tests/qt/UploadSchedulerTests.cpp`

### Implementation instructions

Implement this as a separately approved design program. Do not start a custom
hierarchy implementation before completing Stage A.

#### Stage A: mandatory COPC/EPT architecture decision

1. Build a small, cancellable prototype using PDAL's `readers.copc` and
   `readers.ept` against local and HTTP-hosted fixtures. Exercise spatial bounds
   and requested resolution, record time to first points, bytes fetched,
   duplicate decoding after camera revisits, and peak decoded memory.

2. Determine what the current PDAL C++ APIs expose beyond query results. In
   particular, verify whether the application can obtain stable hierarchy node
   keys and per-node bounds/error/count information, or whether it must map
   spatial/resolution queries to its own node keys. Do not infer this from the
   existence of a reader stage.

3. Test cancellation and reprioritization during rapid camera changes. A
   usable adapter must stop or de-prioritize obsolete requests and must impose a
   strict limit on concurrent decode work and its temporary memory.

4. Compare three explicit outcomes in `HIERARCHICAL_POINT_SOURCE_DESIGN.md`:

   - use PDAL bounds/resolution queries behind an application cache;
   - use a focused COPC/EPT hierarchy adapter while retaining PDAL point-field
     conversion;
   - use an application-owned on-disk hierarchy only where source formats or
     measured query behavior require it.

5. Prefer COPC for newly prepared very large single-file datasets unless the
   measurements show a project-specific blocker. Retain EPT ingestion for
   existing tiled/remote deployments. Record format/version support, remote
   range-request behavior, dependency implications, and failure recovery.

6. Define a fallback for flat LAS/LAZ and other sequential inputs. The fallback
   must either convert to COPC/EPT, create a versioned temporary on-disk block
   cache during import, or clearly cap the supported in-memory size. It must not
   claim out-of-core behavior while retaining every decoded vector.

7. Make the decision document a release gate. A bespoke octree is acceptable
   only when the document contains measured reasons that the COPC/EPT options
   cannot satisfy the interaction, licensing, deployment, or performance
   requirements.

#### Stage B: reloadable source and bounded decoded-CPU cache

1. Introduce a `PointCloudDataSource` interface whose lightweight metadata can
   enumerate root nodes, lazily describe children, and asynchronously request a
   node's decoded point block. Use stable source/node IDs; do not use
   decoded-block addresses as cache or picking identities.

2. Load hierarchy metadata pages on demand and give them a small explicit
   budget or include them in the CPU cache accounting. Keep root metadata
   resident, but do not materialize the complete COPC/EPT tree at open time.

3. Change `PointCloudScene` to own the data source, layer metadata, hierarchy
   metadata, and cache handles rather than strong references to every decoded
   `PointBlock`. A scene snapshot must not accidentally pin the whole cloud.

4. Add a decoded-block cache with a configurable byte budget and LRU policy.
   Track allocated bytes incrementally. Protect entries only while they are
   required by the current frame, an in-flight upload, or a pick; release pins
   deterministically when that work completes or is cancelled.

5. Evict decoded CPU point vectors when the CPU budget is exceeded. A later
   request reloads the node from COPC/EPT or the on-disk fallback cache. Keep
   only compact hierarchy/source metadata resident.

6. Bound decode concurrency and account for temporary decoder buffers in a
   separate in-flight allowance. Cache size plus in-flight allowance is the
   CPU residency contract; an LRU limit that ignores active decoders is not a
   meaningful peak-memory limit.

7. Publish coarse nodes first for hierarchical sources. For sequential flat
   inputs, continue progressive chunks for immediate feedback while the
   reloadable on-disk representation is built.

#### Stage C: bounded GPU cache and hierarchical selection

1. Give `UploadScheduler` a configurable GPU byte budget. Choose a conservative
   default from a user setting rather than from total point count.

2. Key `ResidentBlock` by stable source/node ID and track:

   - allocated bytes;
   - last-used frame number;
   - last-visible frame number;
   - layer ID;
   - a pin count or current-frame protection flag.

3. Maintain total and per-layer resident bytes and points incrementally. Do not
   recompute them by scanning the entire map.

4. Replace `retainReferencedBlocks()` with explicit residency operations based
   on stable node IDs:

   ```cpp
   void beginFrame(std::uint64_t frameNumber);
   void touch(NodeId node, bool visible);
   void evictToBudget(std::span<const NodeId> protectedNodes);
   ```

5. Evict non-visible least-recently-used GPU buffers until the byte budget can
   accommodate requested uploads. GPU eviction and decoded-CPU eviction are
   independent: a GPU-resident buffer does not require its CPU source vector to
   remain cached after upload completes.

6. Build render candidates before scheduling uploads. Prioritize candidates by:

   1. inside the current frustum;
   2. projected screen size;
   3. distance;
   4. recently visible status.

7. Never upload offscreen blocks while visible candidates are missing, unless
   an optional prefetch budget remains.

8. Represent every hierarchy node with stable ID, bounds, geometric error,
   point count, child references, and a source-provided representative point
   block. Ingest this structure from COPC/EPT where possible instead of
   rebuilding it after every open.

9. Move visibility and budget selection into a platform-independent
   `RenderSelection` component. Given camera parameters, viewport size, node
   metadata, and a point budget, it should return selected node IDs ordered for
   upload and drawing.

10. Refine a node only when its projected geometric error exceeds a threshold
   and its children fit within the point/residency budgets. Otherwise draw its
   representative block.

11. Keep coarse root levels resident where practical. Camera motion should
   replace coarse nodes with detailed children progressively, never with empty
   space.

12. Add hysteresis to refinement and eviction so small camera changes do not
   continuously swap nodes.

### Completed residency controls (2026-07-16)

1. One document-owned coordinator now divides the decoded cache budget fairly
   between visible hierarchical layers; hidden layers retain only their coarse
   roots and layer changes trigger immediate rebalancing.
2. Background node queries and concurrent initial COPC/EPT root imports now use
   one application-owned FIFO admission queue, retained across transactional
   document replacement, with a default global limit of two active decodes.
3. Unit coverage verifies allocation, visibility rebalancing, FIFO order, and
   that two scene workers cannot exceed the global decode limit.
4. The Release A instrumentation subset is implemented: cache hits/misses and
   evictions, source query outcomes and decoded-byte volume, decode queue and
   estimated allowance pressure, current/peak cache residency, and
   cross-platform current/peak process RSS are published through
   `RenderMetrics`. `pci_load_bench` also reports process RSS and its high-water
   mark.

### Remaining production qualification

The following measurement and deployment gates remain:

1. Run the implemented `pci_residency_bench` against a representative large
   local production file and archive its cache, decoder, cancellation, reload,
   and RSS result. HTTP and fetched-byte qualification remain outside the
   current local-file scope.
2. The deterministic suite now exercises a decoded CPU working set greater
   than 20× its cache and greater-than-10× cumulative GPU residency churn while
   revisiting evicted nodes. Repeat the same qualification with representative
   local data; automated synthetic coverage is not a substitute for PDAL and
   allocator measurements on that data.
3. Keep the capacity-growth and residency GPU stress cases in the required
   validation-enabled real-device lane. The local native Metal lane passes;
   standing up or changing a dedicated CI runner remains deployment work.

### Tests

- Unit-test node selection at different camera distances and viewport sizes.
- Verify that selection never exceeds its point and residency budgets.
- Verify parent/child hysteresis around the refinement threshold.
- Verify that neither cache evicts a node protected by the current frame,
  decoder, upload, or pick.
- Verify LRU ordering and accurate CPU/GPU byte and point counters.
- Revisit nodes repeatedly and verify cache hits and reload behavior.
- Cancel requests during rapid camera motion and verify concurrency and
  temporary memory remain bounded.
- Add a stress fixture whose decoded size exceeds both budgets by at least an
  order of magnitude.
- Exercise local and HTTP COPC/EPT fixtures without downloading the entire
  source before showing a coarse view.

### Acceptance criteria

- Decoded CPU memory stays within its configured cache budget plus the measured
  and bounded in-flight decoder allowance.
- Hierarchy metadata is paged and bounded; opening a source does not
  materialize metadata for every leaf node.
- GPU buffer memory stays within its configured residency budget plus explicitly
  documented fixed renderer allocations.
- Opening and navigating a cloud much larger than RAM does not retain all
  decoded `GpuPoint` vectors. Steady-state process memory is governed by the
  CPU/GPU cache contracts and compact source metadata, not total point count.
- Every visible region has at least a coarse representation.
- Uploads prioritize visible detail and recover smoothly after rapid camera
  movement.
- The selection algorithm is unit-testable without a GPU.
- The COPC/EPT/custom-source decision is documented with prototype measurements
  before hierarchy implementation begins.

## 3. Render on demand instead of continuously while idle

**Priority:** Release 1, highest routine-user impact.

**Status:** completed 2026-07-17. The viewport now stops scheduling frames
when navigation, uploads, picks, scene invalidation, and smoke-test work are
settled. Flat block publication and hierarchy decode completion share a
thread-safe RAII scene subscription, worker notifications are coalesced onto
the GUI thread, and document/layer mutations explicitly request one frame.
Native Metal coverage verifies that the submitted-frame counter remains
stable while idle and advances when a worker publishes a block.

### Problem

`RenderViewportWidget::render()` calls `update()` unconditionally. This keeps
the render loop and GPU active even when the camera and scene are unchanged.
The continuous loop is currently also used to poll document/scene changes and
to drive keyboard navigation, uploads, and asynchronous picking.

### Files to edit

- `src/renderer/RenderViewport.h`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/renderer/rhi/UploadScheduler.h`
- `src/app/MainWindow.cpp`
- `src/import/PointCloudLoadController.h`
- `src/import/PointCloudLoadController.cpp`
- `src/scene/PointCloudScene.h`
- `src/scene/PointCloudScene.cpp`
- `src/core/NavigationInputState.h`
- `src/core/NavigationInputState.cpp`
- `tests/ui/RendererContractTests.cpp`
- `tests/qt/PointCloudLoadControllerTests.cpp`

### Implementation instructions

1. Add a render invalidation method to `RenderViewport`, for example
   `requestRender()`. `RenderViewportWidget` should implement it by calling
   `update()`; Qt will coalesce repeated requests.

2. Call `requestRender()` after layer visibility, colour mode, removal,
   isolation, and show-all changes in `MainWindow`.

3. Add a thread-safe scene invalidation subscription that covers flat block
   publication, hierarchical node completion/cancellation, and decode failure.
   Bridge worker notifications to the GUI thread with queued Qt delivery and
   request a frame rather than waiting for a revision poll. This notification
   must remain usable after `PointCloudLoadController` has completed the initial
   import, because hierarchical refinement continues during navigation.

4. Add queries for ongoing work:

   - `NavigationInputState::hasMovement()`;
   - pending selected uploads from the current frame plan;
   - `PointPicker::inFlight()`;
   - a queued scene invalidation that has not rendered yet;
   - pending smoke-test frames.

5. At the end of `render()`, call `update()` only while at least one ongoing
   activity requires another frame.

6. Keyboard press should start continuous updates; key release or focus loss
   should allow the loop to stop when no other activity remains.

7. Keep requesting frames while a readback is in flight. QRhi asynchronous
   readbacks may need additional frame submissions before completion.

8. If block notifications are too frequent, throttle them on the GUI thread to
   one pending update request. Do not sleep or throttle the import worker.

### Tests

- Extract the continuation decision into a small pure function and test every
  activity combination.
- Verify that layer mutations request a render.
- Verify that keyboard navigation continues rendering until the key is
  released.
- Verify that an in-flight pick continues rendering until its callback fires.
- Add an instrumentation-only test that a settled viewport stops increasing
  its submitted-frame count.

### Acceptance criteria

- A static viewport does not continuously submit frames.
- Loading, navigation, resizing, layer changes, and picking still update
  promptly.
- Idle CPU and GPU usage drops to near zero.

## 4. Publish useful content progressively during import

**Priority:** Release 1, highest routine-user impact.

**Status:** completed 2026-07-17 for local flat LAS/LAZ. Immutable upload
chunks are capped at 65,536 points (1 MiB of 16-byte vertices). Dense cells
seal at that threshold; every 65,536 sampled points the loader also flushes
only the largest remaining active cell, bounding sparse-stream latency without
turning every tiny cell into a GPU buffer. Final partial cells still publish
before the loader reports completion. Progressive replacements swap their
scene shell in early and retain the previous document for cancellation/failure
rollback.

### Problem

The partitioner targets approximately 262,144 points per spatial cell but only
seals a block at 1,048,576 points or in `finish()`. Most normally populated
cells are therefore not visible to the renderer until the whole import ends.

### Files to edit

- `src/scene/PointBlock.h`
- `src/scene/BlockPartitioner.h`
- `src/scene/BlockPartitioner.cpp`
- `src/import/pdal/PdalPointCloudLoader.cpp`
- `src/scene/PointCloudScene.h`
- `src/scene/PointCloudScene.cpp`
- `tests/unit/BlockPartitionerTests.cpp`
- `tests/component/PdalImportTests.cpp`

### Implementation instructions

1. Separate quantization-cell size from upload-chunk size. A cell may emit
   multiple immutable point blocks that share an origin and scale.

2. Reduce the block sealing threshold to a streaming-friendly size, initially
   64K or 128K points. Benchmark both upload efficiency and time-to-first-frame
   before selecting the final default.

3. Continue to seal all remaining partial blocks in `finish()`.

4. If sparse data still delays first output, add a periodic flush policy based
   on sampled point count. Flush a bounded number of the largest active cells,
   not every tiny cell, to avoid producing thousands of very small buffers.

5. Keep the Release 1 sequential-import change independent of hierarchy work.
   Once item 2 has selected a hierarchical source architecture, use its coarse
   nodes for first publication instead of building a second preview hierarchy
   in `BlockPartitioner`.

6. Ensure block publication and scene revision increments happen before the
   corresponding controller notification is queued.

7. Use the scene-level invalidation mechanism shared with item 3. Hierarchical
   node completion must use the same path even though it occurs after initial
   import completion.

### Tests

- Add points without calling `finish()` and verify that at least one block is
  published after the streaming threshold.
- Verify multiple chunks from one cell preserve all input points and use the
  same quantization transform.
- Verify final partial chunks are published by `finish()`.
- Add a component test proving that scene revision and point count increase
  before the PDAL load completes.

### Acceptance criteria

- Large imports display useful content well before the reader reaches EOF.
- COPC/EPT sources display a coarse spatially representative view before
  full-resolution nodes are requested once the hierarchical source release is
  implemented.
- Block sizes stay within the selected upload-friendly range.
- Progressive publication does not lose or duplicate points.

## 5. Remove repeated per-frame copies, locks, scans, and distance work

**Status (2026-07-17): completed for Release C.** A single consistent
`PointCloudSceneSnapshot` now carries stable flat-block metadata and explicit
count semantics. Hierarchical snapshots contain no decoded payload handles;
payloads are leased only by the current frame selection. The scene revision
fast path is lock-free, snapshots are rebuilt only after a revision change,
decoded eviction also advances the revision, block distance is cached before
sorting, and `UploadScheduler` maintains O(1) resident counters. Flat frame
plans are reused when camera, viewport, document, budget, and scene revisions
are unchanged; hierarchy plans remain current-frame objects by design.

### Problem

Every frame currently copies the document's layers, locks and copies every
scene block vector, rebuilds referenced/upload lists, repeatedly scans resident
blocks for counts, and sorts visible blocks while recomputing square-root
distances inside the comparator.

### Files to edit or add

- `src/scene/PointCloudScene.h`
- `src/scene/PointCloudScene.cpp`
- `src/scene/PointCloudDocument.h`
- `src/scene/PointCloudDocument.cpp`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/renderer/rhi/UploadScheduler.h`
- `src/renderer/rhi/UploadScheduler.cpp`
- Add `src/scene/PointCloudSceneSnapshot.h`
- Extend scene and renderer unit tests.

### Implementation instructions

1. Add a single `PointCloudScene::snapshot()` operation that acquires the
   scene mutex once and returns consistent immutable metadata containing:

   - revision;
   - stable flat block IDs or hierarchy node descriptors;
   - point count;
   - bounds;
   - intensity range;
   - loading-complete state.

   Flat snapshots may retain their capped scene-owned block handles. Persistent
   hierarchical snapshots must contain only stable IDs and compact metadata;
   decoded payload leases belong exclusively to a current `FramePlan` or
   in-flight pick. A cached snapshot must never inhibit decoded-cache eviction.

2. Cache the latest snapshot per layer in `RenderViewportWidget`, keyed by
   layer ID and scene revision. Rebuild cached block metadata only when the
   revision changes.

3. Cache block/node center, radius, estimated point count, byte size where
   known, and hierarchy metadata alongside stable identity. Resolve decoded
   payloads only while constructing the current frame. Do not duplicate point
   vectors or preserve cache leases in renderer snapshots.

4. Store `distanceSquared` in each visible candidate once. Sort the stored
   numeric key instead of repeatedly calculating a center and square root.

5. Maintain resident counts incrementally in `UploadScheduler`. Make the count
   getters O(1).

6. Replace repeated `scene->bounds()` and `intensityMaximum()` calls inside the
   block loop with values from the layer snapshot.

7. Once event-driven rendering is implemented, reuse the last frame plan when
   neither camera revision nor document/scene revisions changed.

### Tests

- Verify snapshots are internally consistent while blocks are appended from a
  worker thread.
- Verify cached layer metadata is refreshed only after revision changes.
- Verify squared-distance ordering matches the existing near-to-far order.
- Verify resident counters remain correct across upload, eviction, layer
  removal, and QRhi recreation.

### Acceptance criteria

- Stable frames do not copy every block pointer or rescan the residency map.
- Scene locking is reduced to one short snapshot acquisition per changed
  revision.
- After item 2, retaining a renderer snapshot does not prevent decoded-CPU
  cache eviction.
- CPU frame-planning time is exposed in metrics and remains stable as resident
  block count grows.

## 6. Batch uploads and uniform updates

**Status (2026-07-17): completed for Release C.** Point-buffer uploads share a
resource-update batch until QRhi reports that it has reached optimal capacity,
with the final non-empty batch submitted once. Uniform records are staged into
one zero-initialized, alignment-aware byte range and uploaded with one dynamic
buffer update. Upload throttling and coarse-to-detail protection are unchanged,
and operation/batch counts are exposed in metrics and real-backend tests.

### Problem

`UploadScheduler` currently obtains and submits one
`QRhiResourceUpdateBatch` per block. `PointCloudRenderer` issues one
`updateDynamicBuffer()` operation per draw record. This creates avoidable QRhi
and backend command overhead.

### Files to edit

- `src/renderer/rhi/UploadScheduler.cpp`
- `src/renderer/rhi/PointCloudRenderer.h`
- `src/renderer/rhi/PointCloudRenderer.cpp`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `tests/qt/UploadSchedulerTests.cpp`

### Implementation instructions

1. Create one resource update batch for block uploads at the beginning of each
   upload group.

2. Add all scheduled `uploadStaticBuffer()` operations to it. When
   `hasOptimalCapacity()` becomes false, submit that batch and start another.

3. Submit the final non-empty batch once. Never submit an empty batch.

4. For uniforms, allocate a byte array sized `uniformStride * drawCount`, zero
   its alignment gaps, and copy each `BlockUniform` to its aligned offset.

5. Update the entire active uniform range with one `updateDynamicBuffer()`
   operation. The project targets Qt 6.7, so use the pointer-and-size overload
   rather than the newer Qt 6.10 move-taking overload.

6. Keep upload and uniform batches separate initially because picking needs the
   uniforms before its render pass. They can be coordinated later if profiling
   proves it beneficial.

7. Record update operation counts in debug metrics so batching can be verified
   in real workloads.

### Tests

- Extend upload planning tests to cover batch boundaries and byte budgets.
- Test uniform staging layout, including QRhi alignment gaps, without a GPU.
- Run GPU tests with validation enabled on the required CI backends defined in
  item 10. Additional backends remain opt-in until reliable runners exist.

### Acceptance criteria

- Normal frames submit one uniform update operation.
- Block uploads use one or a small bounded number of batches per frame.
- Upload byte throttling and first-frame behavior remain unchanged.

## 7. Make adaptive point budgeting stable and measurable

**Status (2026-07-17): completed for Release C.** `RenderMetrics` distinguishes
source, retained-flat, decoded-resident, GPU-resident, requested, selected, and
submitted points. The controller uses a smoothed timing sample, requires
several slow or sustained fast samples before changing quality, adjusts down
faster than up, and ignores empty, upload, and pick frames. Delayed GPU timing
is conservatively excluded for a short cooldown after special work. Metrics
still publish at the normal low-frequency interval and on every settling frame,
and dispatch-to-first-points latency remains persistent in diagnostics.

**Flat-navigation regression correction (2026-07-18): completed.** Progressive
retained-count changes now resize the controller without reconstructing it.
Completed flat scenes settle to every retained point that fits GPU residency;
interactive timing may reduce that budget, while non-interactive settling uses
the full GPU-fit capacity. Reduced flat detail is distributed across a
deterministic, residency-bounded block set rather than a camera-distance cutoff,
so orbiting cannot swap entire spatial regions solely because their distance
ordering changes. Mouse dragging is continuous render activity for timing and
returns to event-driven sleep on release. CPU allocation tests, a greater-than-
one-million-point Metal regression, continuous-drag/idle coverage, and smoke
runs of the 4,538,547- and 7,535,773-point local LAZ fixtures cover the fix.

### Problem

The point budget changes by 10% after a single timing observation. GPU timing
from `lastCompletedGpuTime()` may be zero unless timestamp collection was
enabled and may describe a frame several submissions behind. Upload and pick
work is included in observed frame time, so those operations can incorrectly
reduce the geometry budget. Metrics report the requested budget rather than the
number of points actually submitted.

The document also uses source point count as the expected visible total. That
was already inaccurate for capped flat imports and is now more ambiguous for
hierarchical LOD: source points, decoded representative points across several
levels, GPU-resident points, selected points, and actually submitted points are
different quantities. Scene revisions do not update the document revision, so
the budget can also remain based on stale assumptions.

### Files to edit or add

- `src/core/AdaptivePointBudget.h`
- `src/core/AdaptivePointBudget.cpp`
- `src/renderer/RenderMetrics.h`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/pointcloud/PointCloudMetadata.h`
- `src/import/pdal/PdalPointCloudLoader.cpp`
- `src/scene/PointCloudDocument.cpp`
- `tests/unit/CoreTests.cpp`
- `tests/unit/PointCloudDocumentTests.cpp`
- Add `tests/unit/FrameTimingControllerTests.cpp` if the controller is split
  into its own type.

### Implementation instructions

1. Pass a structured sample into the budget controller:

   ```cpp
   struct FrameSample {
       double frameMilliseconds;
       bool gpuTiming;
       bool includedUploads;
       bool includedPick;
       std::uint64_t submittedPoints;
   };
   ```

2. Smooth valid frame samples with an exponential moving average or a short
   percentile window. Do not react to a single frame.

3. Use asymmetric control: reduce the budget relatively quickly after several
   slow frames, but increase it slowly after sustained headroom.

4. Ignore or separately classify frames with substantial upload or pick work.

5. Treat GPU timestamps as delayed measurements. Do not assume a returned value
   belongs to the immediately preceding frame.

6. Continue supporting the CPU timing path. `QRhiWidget` owns or shares its
   QRhi, so the application cannot assume it was created with
   `QRhi::EnableTimestamps`. If reliable GPU timestamps become a requirement,
   evaluate a renderer that owns its QRhi through a `QWindow`-based path.

7. Return an explicit frame plan from `buildDrawList()` containing selected
   blocks, submitted point count, visible point count, and culled block count.
   Publish actual submitted points in `RenderMetrics`.

8. Define explicit count semantics instead of one expected-selected total:

   - immutable source point count;
   - retained flat sample count, when applicable;
   - decoded CPU resident points/bytes;
   - GPU resident points/bytes;
   - frame-selected points;
   - actually submitted points.

   A hierarchical detail/depth cap is a policy input, not a promise that one
   fixed number of unique points will be retained or drawn.

9. Update totals when scene snapshots change, not only when the document's
   layer configuration revision changes.

### Tests

- Verify that one slow frame does not cause a large budget drop.
- Verify sustained slow/fast frames reduce/increase the budget predictably.
- Verify upload and pick samples do not perturb the geometry controller.
- Verify delayed GPU samples are accepted without assuming frame adjacency.
- Verify flat sampled imports, hierarchical sources, and LOD transitions report
  each count without presenting source or multi-level resident points as actual
  submitted geometry.

### Acceptance criteria

- Point count no longer oscillates visibly around the target frame rate.
- Metrics distinguish CPU/GPU timing and requested/actual point counts.
- Loading and picking do not cause persistent quality reductions.

## 8. Fix large-coordinate normalization and make colour maps data-driven

**Status:** completed. Block-local coordinate normalization shipped on
2026-07-16. Runtime scalar ranges and the data-driven lookup atlas completed on
2026-07-23.

Coordinate colouring
now receives a double-derived block-local affine rather than absolute float
world coordinates. The C++ uniform has field-offset assertions, both vertex
shaders declare the same explicit `std140` layout, and CPU regression coverage
demonstrates distinct normalized values where the former UTM-scale float
reconstruction collapsed adjacent quantization steps.

Colour-map definitions now live in `PointColorMapCatalog.cpp`, which generates
one padded 256-sample RGBA8 atlas at renderer initialization. Continuous and
categorical maps use the same portable 2D texture, and the point shaders no
longer contain map polynomials or palette switches. Each layer has an Auto or
manual scalar domain. Automatic X/Y/Z is the combined full-source domain of
all loaded layers, including hidden layers. Automatic intensity is exact and
source-wide once known, with a stable `[0, 65535]` fallback while flat import
is incomplete or intentionally sampled; it never follows decoded-cache or
retained-sample residency.

### Problem

Coordinate colouring reconstructs large absolute world coordinates as floats
and subtracts similarly large float bounds, which can lose detail for common
geospatial coordinate systems such as UTM. This produces visibly banded or
collapsed local gradients and is not merely cosmetic. It is small and
self-contained enough to fix without waiting for hierarchical rendering.

Separately, Viridis and Turbo were evaluated as polynomials for every rendered
vertex, and categorical colors were embedded as GLSL switch trees. That made a
new map a shader/pipeline concern and offered no runtime scalar-domain control.

### Files to edit or add

- `src/renderer/rhi/PointCloudRenderer.h`
- `src/renderer/rhi/PointCloudRenderer.cpp`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/pointcloud/PointColorPolicy.h/.cpp`
- `src/pointcloud/PointColorMapCatalog.h/.cpp`
- `src/renderer/PointColorMapAtlas.h/.cpp`
- `src/app/PointCloudLayerPanel.h/.cpp`
- `src/scene/PointCloudDocument.h/.cpp`
- `src/scene/PointCloudScene.h/.cpp`
- `src/import/local/LocalPointPageFormat.h/.cpp`
- `src/import/local/LocalPointPageSource.h/.cpp`
- `src/import/local/LocalPointIndexBuilder.cpp`
- `shaders/points.vert`
- `shaders/points.frag`
- `shaders/pick.vert`
- `cmake/PciShaders.cmake`
- Unit, component, Qt renderer, UI, and native-GPU tests.

### Implementation instructions

#### Release 1: coordinate correctness

1. Replace absolute world reconstruction for X/Y/Z normalization with a
   block-local affine transform calculated in double precision on the CPU:

   ```text
   normalized = scalarOffset + quantizedComponent * scalarStep
   scalarOffset = (blockOrigin - layerMinimum) / layerSpan
   scalarStep   = blockScale / layerSpan
   ```

2. Store `scalarOffset` and `scalarStep` in `BlockUniform`. The shader then
   normalizes directly from the 16-bit coordinate without subtracting large
   float values.

3. Keep the C++ `BlockUniform`, `points.vert`, and `pick.vert` uniform layouts
   synchronized. Add static offset assertions for every CPU field, not only a
   total-size assertion.

4. Keep camera/model transforms camera-relative as they are now. This change is
   specifically for scalar colour normalization and should not introduce
   absolute float world positions elsewhere in the shader.

#### Completed follow-up: runtime domains and colour-map catalog

5. Store source/map compatibility, labels, continuous coefficients/stops, and
   categorical palettes in one CPU catalog. Generate three padded atlas rows
   per map so linear filtering cannot bleed between adjacent maps.

6. Bind the atlas once as a clamp-to-edge sampled 2D RGBA8 texture. Continuous
   values address texel centers across a row; categorical values address their
   exact 8-bit texel. Keep the picker layout-compatible and pass it the
   renderer's current bindings at record time.

7. Store an optional manual scalar range in each layer's `PointColorMode`.
   Resolve automatic X/Y/Z from document-wide full source bounds and intensity
   from complete per-source statistics. Persist exact local-page intensity
   statistics in the checksummed manifest. The required min/max fields already
   existed in revision 4, so exposing them does not invalidate page caches.

8. Expose Auto, Min, and Max controls only for continuous sources. A map change
   preserves the range; a source change starts in Auto. Reject incompatible
   maps and non-finite, degenerate manual ranges at the document boundary.

9. Keep one CMake function as the shader inventory used by both the application
   and native-GPU test target.

### Tests

- Unit-test normalization for UTM-scale origins with small local extents.
- Verify endpoints map to 0 and 1 within a tight tolerance.
- Add render-reference tests for every colour source and map where GPU test
  infrastructure permits.
- Verify binding layout compatibility for point and pick pipelines.
- Verify hidden layers remain part of automatic X/Y/Z domains.
- Verify manual ranges remain per-layer and automatic intensity does not change
  as hierarchy pages enter or leave residency.

### Acceptance criteria

- Coordinate colour gradients retain local detail at large world coordinates.
- The UTM-scale regression test fails on the old absolute-float calculation and
  passes with the block-local affine calculation.
- Viridis and Turbo no longer execute high-order polynomials per point.
- Adding a map requires catalog data and tests, not shader or pipeline edits.
- Runtime ranges update immediately, and coordinate defaults cover all loaded
  point clouds rather than only the selected or visible layer.

## 9. Reduce the geometry processed during picking

**Implementation status (2026-07-18): complete.** The viewport constructs an
analytically equivalent narrow camera ray/cone from the requested pixel and
tests it conservatively against the bounds of the currently selected logical
blocks. Candidates retain their full-frame dynamic-uniform index and global
`idBase`, are sorted front-to-back, and are the only blocks submitted to the
existing asynchronous pick pass. `PickRequest` now captures camera, document,
selection, and input generations. Pick metrics expose candidate/input blocks
and points. Picking intentionally targets the representative points currently
displayed; it does not trigger detail decoding.

### Problem

The pick target and readback region are small, but the pick pass still submits
the full selected draw list. Every selected point therefore executes a second
vertex shader whenever a pick is requested.

### Files to edit or add

- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/renderer/rhi/PointPicker.h`
- `src/renderer/rhi/PointPicker.cpp`
- `src/core/NavigationInputState.h`
- `src/core/NavigationInputState.cpp`
- Reuse `src/renderer/RenderSelection.h/.cpp`
- Extend `tests/qt/RenderSelectionTests.cpp`
- Extend `tests/gpu/PointPickerGpuTests.cpp`

### Implementation instructions

1. Convert the requested screen pixel into a narrow world-space pick frustum or
   ray using the inverse view-projection transform, or the equivalent camera
   basis/FOV construction used by the implementation.

2. Query the `PointCloudDataSource` hierarchy selected in item 2 for nodes
   intersecting that pick volume. For COPC/EPT, reuse source node metadata and
   stable IDs rather than constructing a second renderer-only spatial index.
   Expand bounds by a conservative world-space point radius so near-edge
   points are not rejected.

3. Build a pick draw list from only those candidate blocks. Preserve the same
   `idBase` mapping used by pick resolution.

4. Sort candidates front-to-back. If a conservative CPU intersection can prove
   that a nearer block fully resolves the pick, allow later candidates to be
   skipped; otherwise rely on the GPU depth buffer.

5. Include document/selection generation in `PickRequest`, not only camera
   revision. A result from a removed or replaced layer should be marked stale.

6. Keep the asynchronous readback and small readback rectangle. They are sound
   parts of the existing design.

7. After LOD is introduced, decide whether picking selects only displayed
   representative points or requests finer candidate nodes on demand. Account
   for the latter in the CPU cache and decode concurrency budgets and cancel it
   when the pick becomes stale. Make the behavior explicit in the UI contract.

### Tests

- Unit-test screen-ray and pick-frustum construction.
- Verify blocks outside the pick volume are excluded.
- Verify candidates near frustum edges are conservatively retained.
- Verify document changes invalidate an in-flight result.
- Extend GPU tests to cover overlapping blocks at different depths and more
  than one LOD level.

### Acceptance criteria

- Pick vertex work scales with nearby candidate blocks instead of all visible
  points.
- Picking remains asynchronous and produces identical results for existing
  fixtures.
- Stale results cannot target removed or replaced data.

## 10. Make graphics backend selection configurable and more portable

**Implementation status (2026-07-18): complete in the repository.** Runtime
selection accepts `auto`, Metal, Vulkan, D3D11, D3D12, and OpenGL. `auto`
does not call `QRhiWidget::setApi`; explicit choices are platform-validated
and applied before visibility. Requested/selected APIs and validation state
are reported in metrics and startup diagnostics, and failures recommend
`--graphics-api auto`. The `native-gpu` preset is the validation-enabled
platform-primary lane; the same CMake lane can be reconfigured for optional
fallback APIs. Only the locally operated Metal lane is claimed as validated
today—no hosted portable runner is fabricated by this change.

### Problem

The application forces Metal on macOS, D3D12 on Windows, and Vulkan everywhere
else. This rejects otherwise usable systems where Qt's more conservative
defaults—D3D11 on Windows and OpenGL on other desktop platforms—would work.
QRhiWidget's backend must be selected before the widget is shown, so fallback
needs to be designed at viewport creation rather than attempted in the render
callback.

### Files to edit

- `src/renderer/rhi/BackendPolicy.h`
- `src/renderer/rhi/BackendPolicy.cpp`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/renderer/RenderViewport.h`
- `src/core/AppOptions.h`
- `src/core/AppOptions.cpp`
- `main.cpp`
- `tests/qt/BackendPolicyTests.cpp`
- GPU CI configuration when it is added.

### Implementation instructions

1. Add a `GraphicsApi` application option with values:

   ```text
   auto, metal, vulkan, d3d11, d3d12, opengl
   ```

2. Pass the selection through `createRenderViewport()` into the widget
   constructor.

3. For `auto`, do not call `QRhiWidget::setApi()`; let Qt select its documented
   platform default.

4. For an explicit value, validate that it is meaningful on the current
   platform and call `setApi()` in the widget constructor before it enters a
   visible widget hierarchy.

5. Include the requested and selected backend in diagnostics and metrics.

6. On `renderFailed`, provide a clear message suggesting `--graphics-api auto`
   or a known fallback. A transparent automatic fallback would require
   destroying and recreating the entire QRhiWidget before use; do not attempt to
   switch the API on an already initialized widget.

7. Add an optional debug/validation-layer flag for development and GPU CI.

8. Establish only the GPU lanes the project can operate reliably at first:

   - the primary development/deployment backend (currently macOS Metal);
   - one portable fallback with a real runner, chosen from Linux Vulkan,
     Linux OpenGL, or Windows D3D11 according to runner availability.

   Keep D3D12 and the remaining APIs available as opt-in/manual tests. Promote
   another backend to required CI only after its runner is stable; do not block
   renderer improvements on a five-backend matrix.

### Tests

- Unit-test option parsing and platform validation.
- Verify `auto` does not override the Qt default.
- Retain mapping tests between QRhiWidget API names and QRhi implementations.
- Run smoke and picking tests on the two required GPU lanes. Run Metal, D3D11,
  D3D12, Vulkan, and OpenGL as opt-in coverage wherever additional runners and
  drivers are available.

### Acceptance criteria

- Systems without Vulkan or D3D12 can use a supported alternative.
- Explicit backend selection is deterministic and visible in diagnostics.
- Backend failures give an actionable recovery instruction.
- Missing CI capacity for an optional backend does not gate unrelated render
  pipeline work.

## 11. Add render-pipeline instrumentation and scalability tests

**Implementation status (2026-07-18): Release D instrumentation and stress
harness complete.**
Cache, source, decode admission, decoded/GPU residency, process RSS, all point
count stages, visible/culled blocks, draw/submitted-frame counts, uniform
capacity and update operations, upload traffic/batches, and snapshot,
selection, upload, and command-recording timings are implemented. Development
builds add QRhi markers around upload, uniform, pick, and main point work.
Real-backend tests cover progressive arrival, idle settling and wake-up,
stable-plan reuse, and the 257/513-block batching and capacity thresholds. The
Release D adds requested/selected backend state, validation state, pick
candidate/input work, a validation-enabled native-GPU preset, and opt-in
`PCI_PROFILE_RENDERING` records suitable for before/after profiling.
The completed stress subset adds deterministic greater-than-20× CPU cache
churn, rapid cancellation, greater-than-10× cumulative native-GPU residency
churn, current/peak GPU residency and eviction counters, and the local-file
`pci_residency_bench` with machine-readable cache/source/decode/cancellation/RSS
measurements. A generated 64,000-point local hierarchy also drives 160 real
PDAL queries and revisits through a greater-than-10× cache working set in
CTest. A representative production-file run remains a Release A qualification
gate rather than an automated-fixture claim.

### Problem

Current metrics show FPS, timing source, point budget, and total points, but do
not show where frame time or memory is going. Most renderer tests use very small
scenes, so block-count, residency, uniform-growth, and streaming regressions can
escape detection.

### Files to edit or add

- `src/renderer/RenderMetrics.h`
- `src/renderer/rhi/RenderViewportWidget.h`
- `src/renderer/rhi/RenderViewportWidget.cpp`
- `src/renderer/rhi/UploadScheduler.h`
- `src/renderer/rhi/PointCloudRenderer.h`
- `src/app/MainWindow.cpp` for displaying optional diagnostics.
- Add `src/renderer/FramePlan.h` if selection results are formalized.
- Add renderer stress and planning tests under `tests/unit`, `tests/qt`, and
  `tests/gpu`.

### Implementation instructions

1. Extend `RenderMetrics` with:

   - actual submitted points;
   - visible and culled blocks;
   - draw calls;
   - decoded-CPU resident bytes and budget;
   - decoder in-flight bytes and request count;
   - GPU resident and uploaded bytes;
   - pending upload bytes;
   - source cache hits, misses, bytes requested, and cancellations where the
     source adapter can report them;
   - CPU scene-snapshot time;
   - CPU selection/sort time;
   - CPU command-recording time;
   - whether the frame included uploads or picking;
   - current LOD threshold and GPU residency budget.

2. Use lightweight `std::chrono::steady_clock` scopes around CPU phases. Publish
   aggregated values at the existing low-frequency metrics interval, not every
   frame through the UI.

3. Add QRhi debug markers around upload, picking, and main point passes in
   development builds when debug markers are enabled.

4. Add deterministic stress tests for:

   - more than 256 and 512 visible blocks;
   - a source whose decoded data exceeds both CPU and GPU budgets;
   - rapid camera movement and residency churn;
   - progressive block arrival during rendering;
   - layer hide/show/remove cycles;
   - document replacement during an asynchronous pick;
   - zero visible layers;
   - source point counts larger than sampled point counts.

5. Add the focused GPU smoke matrix defined in item 10. Keep GPU tests opt-in
   locally and run the two required lanes on dedicated runners with a real
   display/device and validation layers. Do not make all five QRhi APIs a
   prerequisite for shipping the earlier releases.

### Acceptance criteria

- Performance regressions can be attributed to selection, upload, command
  recording, source/decode work, CPU residency, or GPU work.
- Stress tests cover the thresholds where the current small-scene tests stop.
- GPU validation runs on the one or two backends designated as required by
  project CI; other supported APIs retain opt-in smoke coverage.

## 12. Make large local multi-file datasets paged, globally scheduled, and fairly planned

**Implementation status (2026-07-20): Releases E–H are complete for the
local-file scope, including capacity-aware full-detail settlement, shared
document residency/planning, per-source recovery UX, parameterized
qualification tooling, and archived native measurements. A materially
different production dataset still requires its own acceptance run.**

**Scope decision:** this item assumes local files. It does not add HTTP, change
the source formats, or require users to rewrite their inputs. Existing
hierarchical adapters remain supported and should migrate to the shared cache,
scheduler, and planner contracts where that reduces duplication. Ordinary
LAS/LAZ obtains random-access behavior through an application-owned local page
index stored in a cache directory.

### Representative workload and general scaling target

The motivating example is 25 files with approximately 7 million points each:
175 million source points in one document. It is a repeatable stress workload,
not a supported-file-count target or hard-coded upper limit. The implementation
must schedule any number of input files subject only to explicit resource and
metadata constraints. The current path has five separate scaling failures.

1. `--max-points` is a per-file cap. Since 7 million is below the default
   10-million limit, the flat importer retains every point from every file.
   The document CPU cache does not account for or evict those blocks.
2. `MainWindow::loadPointClouds()` dispatches all files immediately and
   `PointCloudLoadController` submits each heavy import to Qt's global thread
   pool. Actual concurrency is accidental, shares a pool with unrelated work,
   and has no byte-based memory admission.
3. A 512 MiB GPU cache holds only 33,554,432 `GpuPoint` vertices, or about 19%
   of this dataset. Complete flat blocks that are not selected have no coarse
   replacement, so bounded GPU residency alone creates missing coverage.
4. Hierarchical layers are selected sequentially against one shared
   `remaining` point budget. Earlier document layers can exhaust the budget
   before later visible layers request or draw even a coarse root.
5. Every hierarchical scene owns a waiting decode thread, while every flat
   import independently publishes revisions, progress, snapshots, and upload
   candidates. Twenty-five layers are manageable as metadata, but they should
   not imply one heavy reader, resident point vector, or worker thread per
   layer.

The target is not to make every source point simultaneously resident. The
target is to keep all sources addressable while process memory, active decode
work, and GPU buffers remain governed by explicit document-wide budgets. Point
payload residency must not grow with total source-point count. Lightweight
catalog and hierarchy metadata will necessarily grow with the number of files
and pages, so it must be measured separately, kept compact, and paged where a
single in-memory catalog would exceed its own documented allowance.

### Architectural decisions and non-goals

1. **One page contract for every large source.** A renderer-visible payload is
   an immutable, reloadable page identified by `(layerId, pageId)`. Flat and
   existing hierarchical inputs may build or discover pages differently, but
   cache ownership, leases, scheduling, selection, upload, and picking must not
   depend on permanently retained flat blocks.
2. **A local sidecar is required for unindexed LAS/LAZ.** An arbitrary LAZ file
   cannot provide efficient repeated spatial queries without either rescanning
   it or building an index. The application performs one progressive scan and
   writes a versioned page store under an application cache directory. It does
   not modify the source file and must work when the source directory is
   read-only.
3. **`--cpu-cache-mb` becomes the total resident decoded-payload budget.** It
   applies to every large source in the document and includes coarse previews.
   Preview use receives a reported sub-cap so coverage cannot consume the whole
   cache. Active decoder allowances are separately measured and explicitly
   capped; no source type receives a hidden per-layer multiplier. The default is
   now `auto`, derived from effective physical RAM, current reclaimable
   headroom, configured GPU residency, active-decode overhead, and protected
   application/system reserves. It refreshes while the viewer runs and never
   shrinks below already pinned flat reservations; an explicit positive MiB
   value remains a stable user override. This intentionally supersedes item
   2's current fixed coarse-root allowance once Release G lands: roots remain
   preferred eviction fallbacks, but their point payloads are resized or
   evicted rather than exceeding the document budget.
4. **`--max-points` remains a per-source fidelity ceiling.** It is not a memory
   budget. Release E may temporarily reduce flat samples further to obey the
   document CPU budget, but it must report that degradation rather than
   silently treating a per-file cap as a global guarantee.
5. **Coverage precedes detail.** Every visible layer or file tile receives a
   bounded coarse representation before any layer spends the discretionary
   detail budget. Document order is never a render priority.
6. **One central scheduler owns heavy work.** Metadata inspection, preview
   generation, index construction, visible-page decode, and speculative
   prefetch have explicit priorities, concurrency, cancellation, and estimated
   byte permits. Per-scene waiting threads are removed after migration.
7. **GPU arena allocation is deferred until measured.** Correct CPU ownership,
   coarse coverage, and global fairness are required. Replacing individual QRhi
   buffers with suballocated arenas is a later optimization only if command and
   resource-allocation measurements justify it.
8. **There is no fixed source-count limit.** Worker limits, byte permits, cache
   budgets, preview sizes, and UI batching are resource controls rather than a
   maximum layer count. The source catalog uses stable IDs and compact records;
   large result lists and progress models are virtualized or paginated. If
   metadata or disk limits prevent admission, report the measured resource
   shortfall instead of rejecting a batch because it crossed a compiled-in
   file-count threshold.

### Target data flow

```text
N local files (25 x 7 million is one benchmark case)
    -> metadata catalog + top-level bounds index
    -> cached per-file page tables and bounded coarse previews
    -> global screen-error/fairness planner
    -> prioritized task queue with worker and byte permits
    -> one document-wide decoded-page cache
    -> prioritized bounded GPU residency
    -> draw and pick from the same immutable frame plan
```

### Files to edit or add

- `src/core/AppOptions.h` and `src/core/AppOptions.cpp`
  - Parse `--cpu-cache-mb auto` and explicit positive MiB overrides. Add
    explicit heavy-worker and index-cache controls only where a stable user
    setting is necessary. Keep defaults conservative and byte based.
- `src/core/SystemMemoryInfo.h`, `src/core/SystemMemoryInfo.cpp`,
  `src/core/MemoryBudgetPolicy.h`, and `src/core/MemoryBudgetPolicy.cpp`
  - Query the supported desktop platforms through their native memory APIs,
    including Linux cgroup limits, and keep the testable sizing policy separate
    from collection. Qt exposes platform identity and storage capacity but no
    public cross-platform physical-memory/headroom API.
- `main.cpp`
  - Describe `--cpu-cache-mb` as the total decoded-page budget and pass one
    scheduler/cache configuration into the application.
- `src/import/PointCloudImport.h`
  - Split metadata/open, preview, and full/index work into explicit request
    states. Carry scheduler priority and estimated memory instead of creating
    independent unbounded jobs.
- `src/import/PointCloudLoadController.h` and
  `src/import/PointCloudLoadController.cpp`
  - Replace direct use of the global Qt thread pool with the shared bounded
    scheduler and a queue of batch source states.
- `src/import/pdal/PdalSourceInspector.*`
  - Support a cheap batch preflight that returns source point counts, bounds,
    driver, dimensions, file size, modification identity, and an estimated
    retained/indexing cost before heavy decode begins.
- `src/import/pdal/PdalPointCloudLoader.cpp`
  - Keep the existing in-memory flat path only as a small-source compatibility
    path. Route large local flat sources through preview/index construction and
    a reloadable page source.
- Add `src/import/local/LocalPointIndexBuilder.h` and
  `src/import/local/LocalPointIndexBuilder.cpp`.
- Add `src/import/local/LocalPointPageSource.h` and
  `src/import/local/LocalPointPageSource.cpp`.
- Add `src/scene/PointPageId.h`, `src/scene/PointPageSource.h`, and
  `src/scene/PointPagePayload.h` if the current node types cannot express a
  source-independent page contract without format-specific assumptions.
- Add `src/scene/DecodedPageCache.h` and
  `src/scene/DecodedPageCache.cpp` by generalizing or replacing
  `DecodedBlockCache`.
- Add `src/scene/PointMemoryBudget.h` and
  `src/scene/PointMemoryBudget.cpp` in Release E so legacy flat reservations,
  hierarchy cache allocation, previews, and active work cannot each interpret
  the same `--cpu-cache-mb` value as an independent allowance.
- Add `src/scene/PointTaskScheduler.h` and
  `src/scene/PointTaskScheduler.cpp`.
- Add `src/scene/PointDatasetIndex.h` and
  `src/scene/PointDatasetIndex.cpp` for the top-level file/layer bounds index.
- `src/scene/PointCloudScene.h` and `src/scene/PointCloudScene.cpp`
  - Replace persistent large flat `blocks_` ownership with source/page metadata
    plus cache lookups. A small-source compatibility scene may remain explicit
    and size limited.
- `src/scene/PointCloudSceneSnapshot.h`
  - Make large-source snapshots metadata-only. They must not retain decoded
    page payloads between frames.
- `src/scene/PointCloudDocument.h` and `src/scene/PointCloudDocument.cpp`
  - Own the source catalog, top-level bounds index, scheduler, one global page
    cache, preview allowance, and layer-priority state.
- Add `src/renderer/FramePlanner.h` and `src/renderer/FramePlanner.cpp`.
- `src/renderer/RenderSelection.h` and `src/renderer/RenderSelection.cpp`
  - Refactor per-source traversal into candidate production that can be merged
    by the global planner.
- `src/renderer/rhi/RenderViewportWidget.h` and
  `src/renderer/rhi/RenderViewportWidget.cpp`
  - Consume one global immutable frame plan and submit its decode/upload
    requests. Remove sequential per-layer budget exhaustion.
- `src/renderer/rhi/UploadScheduler.h` and
  `src/renderer/rhi/UploadScheduler.cpp`
  - Accept page priority and obsolete-request cancellation. Preserve the
    existing byte limit and frame upload throttle.
- `src/app/MainWindow.h`, `src/app/MainWindow.cpp`, and
  `src/app/LoadingProgressModel.*`
  - Present queued, inspecting, preview-ready, indexing, and ready states for a
    batch, with aggregate work rather than completed-file count alone.
- `src/renderer/RenderMetrics.h` and `tools/pci_residency_bench.cpp`
  - Add multi-source queue, preview, index, page-cache, and per-layer coverage
    measurements.
- Add `LOCAL_POINT_PAGE_STORE_DESIGN.md` before committing the on-disk schema.

### Stage A: preflight, safety limits, and bounded batch scheduling

**Status (2026-07-18): complete.** `PointCloudLoadController` now performs a
two-phase batch operation, `PointTaskScheduler` owns two private workers and a
separate active-byte allowance, and `PointMemoryBudget` accounts conservative
flat reservations against the same document budget used to allocate hierarchy
cache residency. Flat reductions use deterministic spatial-cell previews.
Queue/admission/RSS metrics, refusal diagnostics, queued and active
cancellation, failure isolation, and parameterized 1/7/25/100-source tests are
implemented. Full-fidelity access to points omitted by a safety preview still
depends on Release F's local page store.

This stage must ship independently before the on-disk page store. It prevents
the current batch from exhausting memory but is explicitly a sampled fallback,
not the full-fidelity solution.

1. Inspect all requested files before starting heavy reads. Build a batch
   manifest containing input order, source bounds, source points, dimensions,
   source type, and a conservative decoded-byte estimate.
2. Add a private application scheduler with an explicit default of two heavy
   PDAL jobs. Do not rely on `QThreadPool::globalInstance()` for import
   concurrency. Keep metadata inspection separate so all layer shells can
   appear promptly without starting all decoders.
3. Add byte permits alongside worker permits. A job cannot start unless its
   estimated active decoder allocation fits the configured in-flight allowance.
   Record estimate versus observed peak RSS so estimates can be corrected.
4. For the legacy retained-flat path, calculate a document-wide retained-point
   allowance from `--cpu-cache-mb`, current payload sizes, block-capacity
   overhead, and already admitted layers. Divide it deterministically across
   the batch, subject to the existing per-source `--max-points` ceiling.
5. Introduce `PointMemoryBudget` as the temporary bridge to the unified cache.
   Legacy flat preview reservations reduce the bytes available to the existing
   hierarchy coordinator, and hierarchy residency reduces the bytes available
   to new flat previews. The sum of both resident payload classes must not
   exceed the one document budget. Active decoder permits remain a separate
   bounded allowance.
6. Replace source-order stride sampling with a deterministic spatial preview
   sampler when a flat source is reduced. Every file must contribute a preview;
   earlier files must not consume the entire allowance.
7. Refuse a load only when even the minimum previews plus admitted fixed memory
   cannot fit. Report the estimate, configured budget, and recovery actions.
8. Add metrics for queued/running heavy jobs, reserved/active estimated bytes,
   per-file retained caps, total retained flat bytes, and batch RSS peak.
9. Make cancellation remove queued jobs immediately and stop active PDAL
   callbacks. Finishing or failing a job must release both worker and byte
   permits exactly once.

### Stage B: versioned local page store and progressive index construction

1. Write `LOCAL_POINT_PAGE_STORE_DESIGN.md` first. Define:

   - magic, format version, byte order, and schema version;
   - source fingerprint using canonical path, size, modification time, header
     metadata, and a collision-resistant content sample;
   - source CRS, bounds, available dimensions, and scalar ranges;
   - stable page IDs, parent/child relationships, tight bounds, point counts,
     geometric error, payload offsets/sizes, and checksums;
   - payload representation and whether duplicate CPU attributes are actually
     needed after their packed GPU form and block statistics are produced;
   - crash recovery, cancellation, disk-budget eviction, and compatibility
     behavior for older index versions.

2. Store indices under the application cache directory, keyed by the source
   fingerprint. Never require write access beside the source file.
3. Build into a uniquely named temporary directory/file. Commit with an atomic
   rename only after the header, page table, payload checksums, and source
   fingerprint are complete. Cancellation or a crash must leave no valid-looking
   partial index.
4. During the single source scan, maintain bounded spatial accumulators and
   publish a small root preview first. Produce progressively finer pages while
   indexing continues; do not retain the complete source in memory before
   writing pages.
5. Use page sizes compatible with the existing upload throttle, initially no
   larger than `maximumPointsPerBlock`. Benchmark 16K, 32K, and 64K point pages
   before freezing the format.
6. Start with the simplest portable payload that meets measured disk and load
   targets. Do not add a compression dependency without comparing index size,
   construction time, random-page latency, and CPU cost on representative
   files.
7. `LocalPointPageSource` memory-maps or reads only page-table metadata at open,
   then asynchronously decodes requested payloads. Opening an unchanged source
   must not scan all points.
8. Preserve the current progressive behavior while an index is being built:
   previews and committed pages can render, and missing detail falls back to a
   resident ancestor rather than empty space.
9. Keep the old retained-flat path only below a documented small-source byte
   threshold. Its memory must still be reserved through Stage A admission.

### Stage C: one document-wide page cache and priority scheduler

1. Replace per-scene decoded caches with one document-owned cache keyed by
   `(layerId, pageId)`. This removes rounding and fairness artifacts from
   splitting a byte budget between an arbitrary number of per-layer LRUs.
2. Use `shared_ptr<const PointPagePayload>` or an explicit lease as the only pin
   mechanism. The current frame, in-flight upload, and pick may pin pages;
   snapshots, source metadata, pending requests, and completed uploads may not.
3. Account point-vector capacity and any retained attribute payload. Track
   fixed metadata separately. Cache insertion may temporarily exceed budget by
   at most one measured largest page while an eviction victim is chosen.
4. Bound the total coarse-preview sub-allocation inside `--cpu-cache-mb`. If the
   natural roots for all layers exceed it, generate smaller previews; do not
   silently let pinned roots grow without limit as layer count rises.
5. Move node/page work to `PointTaskScheduler`. Use priorities in this order:

   1. missing visible coarse coverage;
   2. visible refinement needed by the current frame;
   3. in-frustum near-term prefetch;
   4. background index construction;
   5. speculative/offscreen work.

6. Coalesce duplicate requests by page key. When camera or visibility state
   changes, remove obsolete queued tasks and request stop on obsolete active
   tasks. A cancelled task must not publish into the cache after its generation
   is obsolete.
7. Remove the `std::jthread` owned by every hierarchical scene once all sources
   use the scheduler. Adding sources should create compact metadata and queue
   entries, not one sleeping operating-system thread per source.
8. Preserve the existing global decode concurrency metrics and extend them with
   priority, source/layer, queued age, estimated bytes, actual decoded bytes,
   and cancellation latency.

### Stage D: top-level file culling and globally fair frame planning

1. Build a document-level R-tree or BVH over layer/source bounds. Cull entire
   files before traversing their internal page hierarchy.
2. Make each visible source produce lightweight page candidates containing
   stable ID, bounds, error, point count, decoded/GPU residency, ancestor
   fallback, and estimated decode/upload bytes. Candidate production may remain
   source-specific; final allocation may not.
3. `FramePlanner` merges candidates across all layers in two passes:

   - **coverage pass:** reserve a drawable coarse page for every visible layer
     or top-level file tile;
   - **detail pass:** spend remaining point, decoded-byte request, GPU byte,
     and upload budgets by projected error, screen area, residency, recency,
     and optional user layer priority.

4. Define deterministic tie-breaking by stable layer/page IDs. Document order
   may be a final tie-breaker only; it must not decide whether a layer receives
   coverage.
5. Retain refinement/coarsening hysteresis and parent-to-child atomicity. A
   parent remains drawn and protected until every visible replacement child is
   decoded and GPU resident.
6. Return one immutable frame plan containing draw pages, decode requests,
   upload requests, CPU leases, GPU protection, pick candidates, and diagnostic
   reasons. Drawing and picking must consume the same generation.
7. Cache the plan while camera, visibility, source revisions, budgets, and
   residency readiness are unchanged. A page completion invalidates only the
   affected plan generation.
8. Allocate detail approximately by projected contribution, not equal bytes
   per layer. The coverage reservation provides fairness; the detail pass can
   legitimately favor a layer occupying most of the screen.

### Stage E: prioritized GPU residency and measured draw efficiency

1. Extend `UploadScheduler` to consume the global plan's ordered page requests
   rather than rebuilding an unordered pending set from all visible flat
   blocks. Drop an obsolete upload before staging if its plan generation is no
   longer current.
2. Preserve the global residency budget, 48 MiB default frame upload throttle,
   stable keys, LRU/visibility history, protected-page rules, and peak metrics.
3. Add per-layer coarse-resident and detail-resident counters so tests can prove
   coverage independently of total GPU points.
4. Measure draw calls, QRhi buffer count, CPU command time, resource creation,
   and driver memory on parameterized multi-file workloads. Retain the
   25-by-7-million case as one representative comparison point.
5. Prototype GPU arenas only if those measurements show that hundreds of
   individual page buffers or draw calls are a material bottleneck. An arena
   design must retain stable suballocation identities, byte-accurate eviction,
   safe deferred destruction, and support on all required QRhi backends.

### Stage F: batch UX, observability, and recovery

**Status (2026-07-20): complete for the local-file scope.** Renderer/import
progress distinguishes preview rendered, measured full-detail warming, and
final display ready. The loading overlay remains active until an eligible
finite local leaf set completes its atomic GPU cutover. The durable per-source
activity list, weighted aggregate progress, out-of-order preview admission,
disk/index/cache/scheduler/RSS diagnostics, interactive reprioritization,
cancellation, retry, dismissal, and isolated failure recovery are implemented.

1. Represent each source as a state machine: queued, inspecting, preview-ready,
   indexing, ready, failed, cancelled, or stale-index rebuild required.
2. Aggregate progress by weighted work—inspection, source scan/index pages,
   visible preview, and ready sources—not only `finishedFiles / totalFiles`.
3. Admit all metadata shells without waiting for a slow earlier file's preview.
   Preserve user-facing layer order separately from work-completion order.
4. Allow the user to hide, remove, or reprioritize a layer while other files
   continue indexing. Removal cancels queued work and releases cache entries and
   disk-build ownership for that layer.
5. Report when Release E safety sampling is active, when a persistent index is
   being built, index disk usage, CPU/GPU budgets, active worker/byte permits,
   sources with coarse coverage, and sources awaiting coverage.
6. A failure in one file remains isolated. Other sources continue, partial
   index files are discarded, and a later retry can reuse already valid indices.

### Tests

- Add a deterministic, parameterized source manifest. Use 25 sources with 7
  million logical points each as the primary payload-residency regression, and
  larger metadata-only source counts to catch per-source thread, queue, and UI
  scaling mistakes. Payload generation must remain synthetic/lazy so CI does
  not allocate all logical points merely to describe a workload.
- Verify Stage A never exceeds the configured running-job count or reserved
  decoder bytes and that its aggregate retained sample stays under the document
  allowance plus measured block-capacity overhead.
- Verify every visible layer receives a preview even when the total detail
  budget is smaller than the sum of preferred roots. Run this property over
  several parameterized layer counts, including the 25-source regression case.
- Verify source/layer input order does not change coverage or global detail
  selection, apart from explicitly documented stable tie-breaking.
- Verify hidden and offscreen layers release detailed CPU and GPU pages while
  retaining only their bounded metadata/preview allowance.
- Verify the local index is deterministic, invalidated by source changes,
  rejected on checksum/schema mismatch, crash safe, cancellation safe, and
  reusable without a full source scan.
- Verify index construction keeps spatial accumulators, pending page payloads,
  and writer buffers within their documented byte allowances; index size may
  grow with source size, but builder RSS may not.
- Interrupt index construction at every commit boundary and verify that reopen
  chooses either the previous valid index or a clean rebuild, never a partial
  page table.
- Revisit pages across a greater-than-10x working set and verify cache reloads,
  stable process RSS, and no growth proportional to cumulative visits.
- Verify duplicate page requests coalesce and obsolete camera work cancels
  promptly without publishing stale payloads.
- Verify scheduler priority: missing coarse coverage overtakes refinement,
  visible refinement overtakes background indexing, and queued low-priority
  work eventually progresses when higher priorities settle.
- Add native-GPU coverage with parameterized layer counts, constrained CPU/GPU
  budgets, rapid camera movement, hide/show/remove cycles, and complete
  parent-child transitions with validation enabled. Keep 25 layers as a
  representative required case, not a boundary condition.
- Add a multi-file benchmark mode that records inspection time, first-any and
  first-all preview time, index throughput, disk-cache size, page latency,
  scheduler queue/byte peaks, CPU/GPU residency, draw calls, frame phases, and
  current/peak/final process RSS.
- Retain all existing flat small-source, hierarchy, picking, backend, and
  progressive-publication tests during migration.

### Acceptance criteria

- Opening a multi-file dataset never retains decoded point vectors proportional
  to the total source-point count. A lazy 25-by-7-million logical workload is a
  required regression case; using 25 corresponding physical files is one
  optional representative comparison, not an upper limit or release target.
- Current decoded-page bytes, including coarse previews, stay within
  `--cpu-cache-mb`; a cache insertion may exceed it only by one measured largest
  page. Preview use has a reported sub-cap, and active decoder allowances have
  a separate documented limit.
- Heavy PDAL work never exceeds the configured worker count or byte permits.
  Queuing N sources does not create N heavy readers or N scene worker threads.
- GPU point buffers stay within `--gpu-cache-mb` plus documented fixed renderer
  allocations and bounded staging.
- Every visible layer has a drawable coarse representation before any layer
  receives discretionary detail. Reordering the input files does not make a
  layer disappear.
- Hidden/offscreen detail is evictable from both CPU and GPU memory, and revisit
  restores it without a source-wide rescan.
- A completed valid local index reopens without reading every source point.
  Source changes, schema changes, cancellation, disk-full errors, and crashes
  cannot make a stale or partial index appear valid.
- Camera motion cancels obsolete work; queue length and process RSS reach a
  steady range under repeated navigation instead of growing with cumulative
  requests.
- The UI shows useful previews while indexing continues and clearly reports
  reduced Release E sampling until full page-backed access is available.
- The parameterized source-count stress suite and a representative physical
  local-file benchmark selected for the release pass on their required lanes
  before the application claims large multi-file workloads as production
  supported on that machine/data class. A separate 25-file physical dataset is
  qualified when it is a real deployment workload, not because 25 is a special
  architectural boundary.

## Recommended implementation and release order

The release boundaries below are deliberate scope controls. Each release must
leave the main branch in a coherent state and must not depend on unfinished
types from a later release.

### Completed foundation baseline

1. Item 1 is implemented: the picker consumes current layout-compatible
   bindings, resource replacement is transactional, and 257/513-draw GPU
   regression coverage exists.
2. Item 2's functional MVP is implemented: COPC/EPT query-backed logical nodes,
   byte-bounded decoded and GPU LRUs, cancellable per-layer decode work, stable
   IDs, projected-error LOD, hysteresis, coarse coverage, runtime budgets, and
   residency metrics.
3. Small flat LAS/LAZ remains an explicitly capped in-memory compatibility
   path. Ordinary LAS/LAZ above one million points uses the persistent local
   page store and the same bounded residency machinery as other page sources.

This baseline is coherent and testable, but it is an internal foundation—not
yet the production out-of-core acceptance milestone.

### Release A: residency qualification

**Current scope decision (updated 2026-07-18):** format expansion and HTTP
qualification are deferred. Near-term validation uses local files through the
already-supported hierarchy adapter; this does not promote the hierarchy MVP
to production-qualified out-of-core status.

**Automation status (2026-07-18): complete; representative-data run pending.**
The deterministic CPU and native-GPU stress workloads, peak GPU metrics, local
residency benchmark, generated greater-than-10× local PDAL fixture, CTest
contracts, and documented qualification commands are implemented. Release A is
not complete until the benchmark passes on a representative large local
production file with a working set at least ten times the configured CPU cache.

The optimized generated-fixture reference run on macOS/Apple M4 Pro discovered
160 nodes and a 10.627× working set. It held current cache residency to
1,018,368 bytes under a 1 MiB budget, with a 1,137,408-byte insertion peak,
306 evictions, 160 revisit queries, one cancellation observed in 16.561 ms,
and a 208,460-byte peak decoder allowance. Process RSS rose from 29.3 MiB to a
147.8 MiB sampled peak and ended at 146.3 MiB after 322 PDAL queries. This
passes the application-owned cache contract but deliberately does not turn RSS
into a cache-only claim: PDAL/plugin state, allocator retention, and dataset
scale still require measurement on representative production data.

1. **Completed 2026-07-16:** add a document-wide CPU residency coordinator and
   global FIFO decode-concurrency limit with fair allocation between visible
   layers and root-only allocation for hidden layers.
2. **Completed 2026-07-16:** move the source/cache/decode subset of item 11's
   instrumentation forward so cache activity, cancellations, estimated decoded
   bytes requested/produced, decoder allowance, and peak process memory are
   measurable. Physical HTTP/file bytes remain an explicitly external
   measurement because PDAL does not expose them.
3. **Tooling and deterministic coverage completed 2026-07-18; production data
   pending:** exercise a representative local hierarchical file, including
   cancellation, eviction, and revisits, with a decoded working set at least
   ten times the CPU cache. HTTP is explicitly deferred by the current scope.
4. **Local lane completed 2026-07-18:** run item 1 capacity growth and item 2
   residency stress tests with validation on the native Metal backend. Retain
   this coverage on the required real-device CI runner.

Only after this release should the application claim production-qualified
out-of-core behavior. The functional hierarchy MVP does not need to be
redesigned unless these measurements invalidate the PDAL query adapter.

### Release B: routine-user correctness and responsiveness

**Status (2026-07-17): complete.** All four scoped deliverables are
implemented and covered by CPU, UI, component, and native-GPU tests.

1. **Completed 2026-07-16:** implement item 8's block-local affine coordinate
   normalization and its UTM-scale regression test.
2. **Completed 2026-07-17:** publish flat sequential-import blocks
   progressively (item 4) using 65,536-point chunks and a bounded sparse-cell
   flush.
3. **Completed 2026-07-17:** add thread-safe scene invalidation covering flat
   block publication and hierarchical node completion, then use it to stop
   unconditional idle rendering (item 3).
4. **Completed 2026-07-17:** add the minimum item 7/11 metrics needed to prove
   time-to-first-points, stable submitted-frame count while idle,
   requested/selected/submitted points, draw calls, and uniform-capacity
   behavior. Settling frames force one final metrics publication before the
   viewport sleeps.

### Release C: frame-planning and command efficiency

**Status (2026-07-17): complete.** The scoped work below is implemented and
validated by the warnings-as-errors CPU/Qt suite and the native Metal GPU
suite. The 2026-07-18 flat-navigation correction described in item 7 preserves
this status and adds explicit coverage for the interaction regression.

1. **Completed:** correct the post-hierarchy point-count semantics in item 7:
   source, flat
   retained, decoded resident, GPU resident, selected, and submitted counts are
   different quantities.
2. **Completed:** rework item 5 around stable metadata and current-frame
   payload leases. A
   persistent renderer snapshot must never retain decoded hierarchy payloads.
3. **Completed:** batch uniform and block updates (item 6), preserving the
   upload byte budget
   and coarse-to-detail transition ordering.
4. **Completed:** add the Release C planning, upload, batching, and
   command-recording metrics and debug markers from item 11.

### Release D: interaction, portability, and optional rendering work

**Status (updated 2026-07-24): complete.** Candidate-narrowed asynchronous
picking, stale document/selection protection, configurable startup backend
selection, backend diagnostics, the focused native/optional GPU lane, and
render-profile output are implemented. Later measured work added independently
authored eye-dome lighting and bounded 1–8-pixel hardware point sizing, with
main-UI controls and native-GPU image tests. Circular splats, density-aware
sizing, and MSAA remain future options. The colour-map LUT decision was
revisited on 2026-07-23 for runtime range control and extension simplicity, and
is now complete under item 8.

The recorded validation baseline was a one-million-point synthetic scene on
Metal with validation enabled on an Apple M4 Pro. Its settled sample submitted
1,000,000 points in 16 draws, with 0.818 ms of CPU command recording and a
16.285 ms reported frame sample. The latter is presentation-paced and does not
isolate fragment/vertex shader alternatives; it is therefore evidence for the
profiling path, not evidence that a LUT, splat, or MSAA implementation is
faster. Any remaining optional feature still needs an A/B prototype on
required APIs.

1. **Completed:** reuse the selected logical hierarchy for pick-candidate
   narrowing (item 9), preserving global IDs and sparse uniform offsets.
2. **Completed:** add configurable backend selection and the focused
   required/optional GPU matrix from item 10.
3. **Completed:** expose settled render and pick-work profiles. Colour-map
   lookup moved forward later as data architecture. EDL and bounded manual
   hardware point size subsequently passed native-GPU comparisons; circular
   splats, density-aware point size, and MSAA still require comparative
   evidence.

The backend CLI can be implemented earlier when needed for compatibility;
standing up a five-backend CI matrix is never a prerequisite for Releases A–C.

### Release E: multi-file safety and bounded admission

**Status (2026-07-18): complete. Release F is implemented below.**

This is a self-contained safety release. It prevents a large flat-file batch
from causing uncontrolled concurrent decode or retained-memory growth, even
though it cannot yet expose every source point at arbitrary detail. It adds no
compiled-in maximum file count.

1. **Completed:** inspect the complete batch before starting heavy reads and
   carry source bounds, point count, driver, dimensions, file size,
   modification identity, and conservative memory estimates into each request.
2. **Completed:** replace Qt global-pool fan-out with `PointTaskScheduler`,
   using two private workers, priority/FIFO ordering, explicit active-byte
   permits, and the existing hierarchy decode admission as a temporary adapter.
3. **Completed:** add RAII `PointMemoryBudget` reservations and apply one
   document-wide payload allowance derived from `--cpu-cache-mb`. Flat
   reservations immediately reduce hierarchy cache allocations; conservative
   reservations shrink to measured vector capacity at EOF.
4. **Completed:** calculate a deterministic common fidelity cap across the
   batch, retain the per-source `--max-points` ceiling, and use spatial-cell
   previews instead of source-order stride sampling whenever safety reduction
   is required.
5. **Completed:** expose queued/running/active-byte/reservation/RSS metrics,
   aggregate batch status, explicit budget-shortfall diagnostics, immediate
   queued cancellation, cooperative active cancellation, and isolated source
   failures.
6. **Completed:** add parameterized lazy-source tests at 1, 7, 25, and 100
   sources plus scheduler, byte-accounting, preview, refusal, cancellation,
   failure-isolation, hierarchy-sharing, and real-PDAL spatial-sampling tests.
7. **Completed:** replace the fixed 512 MiB application default with an
   automatically refreshed, platform-aware point-payload budget. Preserve a
   static 70% process envelope, live OS/application/GPU/decode reserves,
   container-aware effective capacity on Linux, a 1 GiB query-failure fallback,
   and the explicit `--cpu-cache-mb <MiB>` override. Unit tests cover policy
   sizing, fallback, native-query coherence, parsing, and safe live ceiling
   changes; the representative two-LAZ run retained all 4,538,547 and
   7,535,773 source points without safety thinning.

Release E is worth shipping alone because it replaces likely OOM or system
thrashing with a predictable bounded preview. The UI must state when safety
sampling has reduced detail. Do not describe this release as full-fidelity
out-of-core LAS/LAZ.

### Release F: persistent local paging for ordinary LAS/LAZ

**Status (updated 2026-07-24): complete for the local-file v1 scope. Releases G
and H are also complete for the available representative local workload.**

1. **Completed:** approve `LOCAL_POINT_PAGE_STORE_DESIGN.md`, including fingerprinting,
   versioning, payload schema, checksums, crash recovery, disk limits, and cache
   invalidation.
2. **Completed:** implement progressive `LocalPointIndexBuilder` construction
   under Qt's platform-native application cache directory. The builder uses a
   bounded root preview, bounded external Morton-sort runs, fixed-size
   Morton-ordered leaves, bounded bottom-up parent sampling, a cross-process
   heartbeat lock, and atomic directory commit. Dense or duplicate coordinates
   cannot enlarge a leaf beyond the configured point limit.
3. **Completed:** implement `LocalPointPageSource` behind the source-independent page
   contract. Publish previews and completed pages during construction, then
   reopen a valid completed index without a source-wide scan. Manifests and
   individual payloads are checksummed; internal descriptors retain complete
   descendant bounds so preview sampling cannot incorrectly cull later detail.
4. **Completed:** route ordinary LAS/LAZ above one million source points through
   local paging. Paged sources use hierarchy admission and therefore receive no
   retained-flat reservation or safety thinning. The retained path remains only
   for smaller inputs whose conservative allocation is admitted by Release E.
5. **Completed:** add deterministic output, single-page completeness, strict
   leaf bounds, manifest/payload corruption, interruption at reading and
   optimization, cancellation cleanup, source invalidation, restart reuse,
   storage-allowance refusal, greater-than-10x page revisits, and a 25-source
   admission regression. Construction proactively checks estimated cache and
   temporary disk demand against the configured allowance and reported free
   space; runtime permission or disk-write failures use the same uncommitted
   cleanup path. A destructive test that fills a host volume is intentionally
   not part of CTest.
6. **Completed:** expose the exact finite leaf working set of a committed local
   page source and add document-level full-residency admission. Admission
   requires every visible layer's exposed point count to equal its immutable
   source count, enforces each layer's decoded-cache allocation, checks
   aggregate CPU and GPU bytes with one-eighth transition headroom, and rejects
   capped or incomplete sources. Eligible leaves are pinned and warmed without
   cancelling camera requests; the renderer preserves its current hierarchy
   until all leaf CPU payloads and GPU buffers are ready, then switches in one
   frame to leaf-only rendering. Adaptive point-budget changes are disabled
   while this mode is eligible. Layer or budget changes release the pins and
   restore normal bounded LOD.
7. **Completed:** separate first-preview and final-display readiness in the
   renderer/UI contract. Keep source-reading progress authoritative while a
   preview renders, report exact leaf decode/upload warming, and close loading
   UI only after full-detail activation when the admitted working set fits.
   Local index scene shells remain incomplete until atomic commit, and cache
   manifests replace the early source-order root with a representative sample
   from the completed hierarchy.

Qualification on the Apple M4 Pro development host used the final v1 build
revision and a Debug executable. A 4,538,547-point LAZ built its 160 MiB cache
entry in 17.4 seconds and reopened in 0.35 seconds. A 7,535,773-point LAZ built
its 272 MiB entry in 29.0 seconds and reopened in 0.69 seconds. Loading both
completed entries together took 0.89 seconds, reported 4,538,547 / 4,538,547
and 7,535,773 / 7,535,773 source points, and rendered successfully through
Metal. The capacity-settlement qualification exposed 12,074,320 exact source
points, warmed the leaf buffers without GPU eviction, and changed from coarse
coverage to the complete visible leaf set in one frame; later frames retained
the same leaf density. Final GPU residency was approximately 186 MiB under the
512 MiB budget. The full development build and all 149 registered unit,
component, Qt, UI, stress, and benchmark tests pass; the native Metal suite
passes 15 cases, including loading-readiness signaling, atomic full-detail
cutover, orbit/zoom density stability, and idle settlement.

Release F makes large single local LAS/LAZ files fully addressable and bounds
their resident payloads. Release G now supplies the document-wide residency,
scheduling, culling, and fair planning layer above those pages.

### Release G: unified residency, centralized decoding, and global planning

**Status (updated 2026-07-24): complete for the local-file scope. Release H's
available representative local workload passed; materially different
production data remains an external qualification gate.**

1. **Completed:** `PointCloudDocument` owns one byte-bounded
   `DecodedPageCache`, keyed by stable source/node identity. Scene snapshots
   remain metadata-only for hierarchical sources; current frames hold explicit
   payload leases. Layer removal drops cache entries and per-source state, and
   root admission/refreshed automatic budgets cannot exceed the shared CPU
   ceiling. When preferred roots no longer fit, every hierarchy root is
   deterministically resampled under an equal discretionary byte cap; a source
   is refused only when the budget cannot retain one preview point per source.
   A cache insertion may peak by one measured immutable page while it selects
   an eviction victim; steady residency remains within budget.
2. **Completed:** all interactive node/page decoding runs through the one
   document `PointTaskScheduler`. Per-scene `std::jthread` workers, condition
   queues, and the duplicate hierarchy-admission gate were removed. Queued work
   is coalesced and cancellable, active work uses cooperative stop tokens, and
   equal-priority sources rotate without retaining scheduler state for removed
   or idle sources. Renderer decode diagnostics now report this scheduler.
3. **Completed:** `PointCloudSourceBoundsIndex` provides a document BVH and is
   refreshed when progressive source bounds change. The renderer rejects whole
   offscreen files before hierarchy traversal; unknown bounds remain
   conservatively visible.
4. **Completed:** `FramePlanner` performs deterministic max-min coarse coverage
   before a screen-contribution-weighted detail pass. Stable layer IDs break
   ties, input order cannot exhaust a later source, and budgets smaller than the
   sum of preferred roots thin every source together. The retained-flat
   compatibility planner likewise reserves one complete GPU block per source
   before detail blocks and uses a navigation-stable spatial sample.
5. **Completed:** `RenderViewportWidget` builds one immutable `FramePlan` for
   the current generation. Its decode requests, ordered uploads, CPU leases,
   GPU protection, draw blocks, and pick candidates share that plan. Existing
   parent-child atomic refinement, LOD hysteresis, pick-generation rejection,
   full-detail atomic cutover, and event-driven idle sleep remain intact.
6. **Completed:** CPU regressions cover a shared cross-source LRU, bounded root
   admission, central scheduler concurrency and rotation, source BVH refresh,
   25-source low-budget coverage, reordered candidates, projected detail,
   rapid request cancellation, and 25-layer hide/show/remove churn. The native
   Metal suite adds a 25-layer GPU-residency case proving every layer retains
   coverage under a one-block-per-layer budget.

Qualification used the Apple M4 Pro development host. The warnings-as-errors
CI build succeeds and all 161 registered non-GPU unit, component, Qt, UI,
stress, and benchmark tests pass. The complete Metal-validation test executable
passes 316 assertions in 12 cases, including 25-layer coverage, live root-buffer
replacement, hierarchy residency churn, full-detail cutover, picking,
navigation, and idle settlement.
The CTest wrapper cannot access the macOS window server inside the command
sandbox, so native cases were executed directly against Metal after the same
`native-gpu` build.

Release G is the point at which the application can technically support the
large multi-file workload without retaining all points or starving later
layers, subject to Release H's representative qualification. Supported scale
is determined by documented budgets and measured metadata overhead, not a file
count constant.

### Release H: multi-file qualification, UX completion, and measured GPU scaling

**Status (2026-07-20): implementation complete for the local-file scope.
Native and available representative-data qualification pass. Production
sign-off for a separate large organizational dataset remains a data-dependent
acceptance run, not an implementation task or source-count limit.**

1. **Completed:** the layer panel has a durable per-source activity list with
   queued, inspection, resource-wait, read, index, preview, ready, failed, and
   cancelled states. Batch progress is weighted by real source work instead of
   file count. Context actions prioritize, cancel, retry, or dismiss a job.
   Per-layer index size/reuse and document-wide CPU/GPU, index, cache, decoder,
   importer, coverage, and process-RSS diagnostics are visible. Later previews
   publish immediately while positional insertion preserves user input order.
2. **Completed:** `pci_multifile_bench` accepts an arbitrary local LAS/LAZ file
   list and uses the production load controller, shared document cache, and
   scheduler. Stable JSON records first-any/first-all preview, import/index and
   request latency, persistent bytes/reuse, CPU residency, process RSS,
   cache/source/decode/import scheduler counters, cancellation, and boundedness.
   CTest includes a forced-local-page LAS/LAZ smoke invocation.
3. **Completed as behavioral contracts:** generated tests cover 25-source fair
   coverage under low budgets, reordered source completion, hide/show/remove
   churn, cancellation and retry, restart/index reuse, source fingerprint
   invalidation, corrupt cache recovery, and disk-admission failure. The
   available 12,074,320-point two-file dataset was measured cold and after
   restart. The earlier 25-by-7-million scenario is explicitly a comparison
   workload, not a target or hard-coded ceiling. It still requires the owner of
   such files to run the parameterized tool before claiming production support
   for that particular allocator and I/O environment.
4. **Completed for the available representative workload:** the native Metal
   lane ran with validation on an Apple M4 Pro with 4 GiB CPU and 512 MiB GPU
   budgets. All 369 finite detail pages became resident in 7.139 seconds; first
   points appeared from any/all sources in 164.0/176.5 ms. CPU page residency
   peaked at 266,355,936 bytes, GPU point residency at 193,713,408 bytes, and
   process RSS
   at 915,095,552 bytes, with no cache or GPU eviction. The settled visible
   frame used 368 draw calls; sampled frame time was 0.371 ms p50, 0.460 ms p95,
   and 0.590 ms maximum. Machine-readable cold, reopen, and native reports plus
   commands and scope notes are archived in `qualification/`.
5. **Measured decision: keep the current GPU allocation design.** The native
   data did not identify QRhi buffer creation, command recording, or frame time
   as a material bottleneck. No speculative GPU arena or additional batching
   layer is introduced. Reopen this decision only when a representative report
   shows buffer count or command recording consuming a material frame share.

The 2026-07-24 warnings-as-errors CI audit passes all 193 registered non-GPU
unit, component, Qt, UI, stress, and benchmark tests. The validation-enabled
native Metal selection passes all 19 CTest entries: 14 focused GPU cases, four
renderer smoke cases, and their fixture generation.

Native qualification also exposed and fixed a full-detail warm-up liveness
edge: a completed request could leave a finite page undiscovered after queued
invalidations coalesced. Warming now retains discovered payloads, polls without
distorting cache hit/miss diagnostics, bounds probes per frame, resubmits a
missing finite set at most three times, and reports an explicit failure instead
of spinning. Qualification output is deferred behind the settling-frame metric
callback, so it records the active full-detail draw list and true display-ready
latency rather than the preceding coarse frame.

The implementation gate is closed. Generated workloads prove contracts and
regressions; representative file and allocator behavior must still be measured
before asserting production support for a materially different dataset or
machine class.

### Dependency and scope summary

- Releases B–D remain complete and are not reopened by item 12.
- The outstanding Release A representative-data gate may proceed alongside
  Release E. It qualifies the existing hierarchy adapter and is not a
  prerequisite for bounded flat-batch safety.
- Release E is complete and is now the immediate OOM guardrail. It deliberately
  reports reduced spatial previews rather than pretending omitted flat points
  remain addressable.
- Releases F and G are the required architectural solution. Neither GPU arenas
  nor a five-backend CI matrix may block them.
- Release H is qualification and measured optimization, not permission to add
  unrelated visual features.

## External references

### Hierarchical point-cloud formats and PDAL

- [PDAL readers, including COPC and EPT](https://pdal.io/en/stable/stages/readers.html)
- [PDAL COPC reader](https://pdal.io/en/stable/stages/readers.copc.html)
- [PDAL EPT reader](https://pdal.io/en/stable/stages/readers.ept.html)
- [COPC specification and format overview](https://copc.io/)

These references establish format and reader capabilities, not the application
architecture. Stage A must still measure the C++ integration, node metadata,
cancellation, duplicate decoding, range requests, and cache behavior required
by an interactive viewer.

### QRhi

- [QRhiWidget lifecycle and continuous rendering](https://doc.qt.io/qt-6/qrhiwidget.html)
- [QRhi shader-resource binding compatibility](https://doc.qt.io/qt-6/qrhishaderresourcebindings.html)
- [QRhi resource update batching](https://doc.qt.io/qt-6/qrhiresourceupdatebatch.html)
- [QRhi command-buffer timing behavior](https://doc.qt.io/qt-6/qrhicommandbuffer.html)

QRhi itself has limited source and binary compatibility guarantees. Continue to
build and deploy against the same Qt version, and run renderer tests whenever
the Qt minor version changes.
