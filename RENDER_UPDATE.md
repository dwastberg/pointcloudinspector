# Rendering Update Plan

Phase numbers are execution order. Each phase is independently shippable and lands with its own tests.

## Goal

Make interactive rendering predictable and smooth for point-cloud documents of 100 million or more points spread across several LAS/LAZ files, while preserving what the tool exists for: fluid navigation, accurate measurement, and exact point data once the user zooms into an area.

Source size should determine storage and cache behavior. It must not determine how many points are submitted in an interactive frame. Frame work should be bounded by what the viewport can resolve, measured frame time, and explicit point, draw, decode, upload, and residency budgets.

## What the display can actually resolve

The premise that a 100M+ document "does not fit" is misleading, and the arithmetic sets every budget below.

- GPU residency ceiling: 512 MiB default (`ApplicationConfig.h:32`) / 16 bytes (`GpuPoint`) = **33.55M resident points**. `buildPlan` already reserves one ninth for parent-to-child transitions, so the usable ceiling is **~29.8M points**.
- 1920x1080 is 2,073,600 pixels. Default point size is 2 px (`RenderViewport.h:22`), so 4 px^2 per point: **~518,000 points fully cover the screen**.
- 3840x2160 is 8,294,400 pixels: **~2,073,000 points**.
- The qualification window is 1280x800 (`ApplicationBootstrap.cpp`), i.e. 1,024,000 pixels: **~256,000 points**.

A 1M-point interactive budget covers 1080p about twice over. We are never short of memory for *display*. We are short of pixels. Every point-count criterion in this plan must therefore be stated relative to viewport pixels, not as an absolute.

## How the hierarchy is actually shaped

This governs all the budget math and was not accounted for in the previous revision.

`LeafPageWriter::mergeRuns` cuts the Morton-sorted point stream into **equal-population chunks of exactly `pointsPerLeaf` points** and names each chunk `nodeIdFromMorton(chunkIndex, level)` (`LeafPageWriter.cpp:226-240`). `ParentHierarchyBuilder` then groups by `parentId(child) = {level-1, x>>1, y>>1, z>>1}`. The result is a balanced octree over the Morton order, not a spatial-cell octree.

Consequences, all exact rather than estimated:

- Every leaf holds exactly **32,768** points (`LocalPointPageFormat.h:25`); every interior node holds exactly **16,384** (`rootPreviewPoints`, `LocalPointPageFormat.h:26`).
- `BlockPartitioner` emits **one block per node** at these sizes, so draws == selected nodes, and a per-draw uniform is a per-node uniform.
- A 19M-point file has 580 leaves and lands at level 4; 113M has 3,449 leaves.
- Refining one node multiplies its point cost by 8 (interior children) or 16 (leaf children).
- Node cost estimates are exact, which makes any priority/cost model trivially accurate.

Two structural consequences that matter:

1. **A 1M budget buys roughly 30 leaf draws or 61 interior draws.** Starting from a 16,384-point root, two full refinement steps reach 1.05M points. Across six sources sharing one budget, each source barely passes level 1. This is why the pixel-derived target in Phase 10 matters, and why Phase 6 exists.
2. **A fully refined idle frame is ~910 draws** (29.8M / 32,768). Thousands of draws is the *correct* answer when idle. Any draw cap must be idle-aware or Phase 6 cannot converge.

### Phantom nodes

Occupied leaf indices are a prefix `0..n-1`, so `nodeIdFromMorton` leaves gaps in id space. For a 580-leaf file, level 1 has only indices 0 and 1 occupied: **six of the root's eight children do not exist.**

`LocalPointPageSource::node()` reports a missing page as `bounds = sourceBounds`, a nonzero `estimatedPointCount`, and no payload (`LocalPointPageSource.cpp:311-320`). So a phantom child always passes the frustum test, is never resident, and `childrenReady` in `RenderSelection::select()` is `all_of(visibleChildren, resident)`. Refinement is therefore blocked until every phantom has been round-tripped through the decode pipeline, where `readNode` returns an empty payload (`LocalPointPageSource.cpp:377-383`) that finally reads as "resident, zero points".

This works, but it costs a decode round-trip per phantom before the root can refine at all, feeds bogus source-sized bounds into every projected-error and culling computation, and re-blocks refinement whenever an empty payload is evicted. Phase 1 fixes it.

## Requirements

1. **Navigation** is the common case. Frame time bounded, LOD changes local.
2. **Measurement** must resolve real source points at source precision.
3. **Zoom-to-exact.** When the user frames a region, every point in it must eventually be drawn.

Requirement 3 is met by correct LOD, not by a whole-document exact mode. At 19 points/m^2 (19M over a 1 km tile):

| Frame point budget | Ground area drawn at full source density |
| --- | --- |
| 1M | ~230 m x 230 m (30 leaves) |
| 4M | ~460 m x 460 m |
| 29.8M (usable ceiling) | ~1.25 km x 1.25 km, the whole tile |

At terrestrial/MLS density of ~5000 points/m^2: 1M covers ~14 m x 14 m, 29.8M covers ~77 m x 77 m.

Phases 1 and 2 deliver requirement 3 for regions under the interactive budget. Phase 6 extends it to the full ceiling.

## Anti-requirements

- Do not admit full detail merely because the document fits in memory.
- Do not pin every leaf node.
- Do not wait for every leaf before leaving root previews.
- Do not upload or protect invisible leaves.
- Do not let one missing subtree collapse every source to its root.
- Do not let budget exhaustion collapse a layer to its root.
- Do not partially draw a node. Select it or do not.
- Do not treat a nonexistent node as an unloaded one.

## Phase 1: Hierarchy node metadata contract

Small, isolated, and a prerequisite for Phases 2, 3, 7.3 and 10. `node()` currently lies about two different things; fix both here, because every later phase reads it as truth.

### 1a. A nonexistent node must not look like an unloaded one

Establish a contract on `PointCloudDataSource::node()`: **a node that does not exist reports invalid bounds and zero estimated points.** `Bounds3d::valid()` already exists and is the natural signal, so `PointCloudNode` needs no new field.

`LocalPointPageSource` can answer this with no format change, because it already holds the full page table — the test is `state_->complete && !state_->pages.contains(id)`. While the store is still building, a missing page stays "unknown", which is the existing behavior and remains correct; `readNode` already blocks on `state_->changed` until the page exists or the store completes.

Then, in `RenderSelection`, exclude children with invalid bounds from `visibleChildren` entirely: not requested, not counted toward `childrenReady`, not counted in `estimatedChildPoints`.

Gains: the root refines on the first frame instead of after six decode round-trips; no source-sized phantom bounds enter projected-error, culling or planning; empty payloads stop occupying decoded-cache slots and stop re-blocking refinement when evicted.

### 1b. A detail-capped level must not report itself as a leaf

`LocalPointPageSource`'s constructor reduces `maximumLevel_` when `maximumPoints < sourcePointCount` (`LocalPointPageSource.cpp:206-221`), and `--max-points` **defaults to 10,000,000** (`ApplicationConfig.cpp:52-58`). The clamp stops at the level where `rootPreviewPoints * 8^L >= maximumPoints`, which is level 4, so a single file above roughly 134M points loses its deepest level(s).

Two problems, both silent:

- `node()` reports `.leaf = id.level == maximumLevel_`, so a clamped level claims to be a leaf while actually being a 16,384-point reservoir page. Phase 2's "leaves are complete", Phase 3's `coverageFactor`, and Phase 6's "reaches full source density" all become false with no signal.
- The `--max-points` help text says "Paged and hierarchical sources retain all points", which this clamp contradicts.

Separate the two concepts: `leaf` means "this node has no children in the store", and a distinct flag or accessor means "this is the deepest level currently served". A clamped node is a non-leaf that cannot refine, and the UI should be able to say so. Fix the help text in the same change.

### Acceptance criteria

- A completed store reports nonexistent nodes as nonexistent; an incomplete store still reports them as unknown.
- Refinement of a node with fewer than eight real children is not gated on the missing ones.
- No request is issued for a node known not to exist.
- A node at a detail-capped level does not report `leaf = true`, and the cap is queryable.
- `PdalHierarchicalPointSource` honours both contracts or documents why it cannot.

Tests: extend `tests/renderer/RenderSelectionTests.cpp` for the traversal behavior, and add page-source coverage for the existence and cap contracts alongside the existing local-page tests.

## Phase 2: Bounded, non-collapsing node selection

The highest-value behavioral fix. It breaks requirement 3 on the exact interaction the user cares most about.

### What is wrong

In `RenderSelection::select()`, budget exhaustion and non-residency return the same signal:

```cpp
if (remaining == 0) {            // RenderSelection.cpp:431
    return false;
}
...
if (!state.resident) {           // RenderSelection.cpp:438
    static_cast<void>(requestNode(state));
    return false;
}
```

A `false` from any child sets `complete = false` in the ancestor, which executes `result.drawNodes.resize(drawStart)` (`RenderSelection.cpp:513`). At the root, `drawStart` is 0.

Note precisely when this bites. Because descent is guarded by `childrenReady`, the `!resident` return is unreachable below the root, so **the rollback path is exclusively a budget-exhaustion mechanism**. At an interior node it coarsens one subtree and *restores* `remaining` to that node's entry value, making budget accounting non-monotonic and letting the traversal reclaim and re-spend. At the root it discards the entire layer selection and draws root-only. That happens whenever the budget runs out on the last-visited node of one root child's subtree while another visible root child remains — a single-frame full-layer flash.

Three further defects in the same function:

- Every node is looked up twice: once while the parent gathers `visibleChildren`, once at the top of its own `selectNode`. `lookup` calls `scene->nodePayload()`, and the payload handle from the first lookup is dropped, so residency can change between the two. `childrenReady` can therefore be computed from stale state.
- `nextRefined.insert(id)` (`RenderSelection.cpp:451`) is not rolled back, so an abandoned refinement is recorded as refined; the next frame uses `coarsenPixelError` (2.0) instead of `refinePixelError` (3.0), refines more eagerly, and fails again.
- Child requests are gated on `estimatedChildPoints <= remaining`, the **draw** budget, and `PointFrameCoordinator` passes `requestPointBudget = layerRemaining`, the same value. A subtree that cannot currently be afforded is therefore never requested, so it can never become resident, so it can never be afforded. Deterministic starvation of later-visited subtrees, and it blocks Phase 6 from converging.

### The fix: reserve before you spend

Do not add a tri-state result. Make exhaustion unreachable instead.

At a node with `remaining` and visible children `C_1..C_n` of own-cost `p_1..p_n` (their `residentPointCount`), keep the existing `sum(p_i) <= remaining` descent guard, then process `C_i` with a cap of `remaining - sum_{j>i} p_j`. Every child is then guaranteed at least enough budget to draw itself.

Consequences, all good:

- No child ever returns "budget exhausted"; the rollback becomes unreachable and is **deleted**.
- `remaining` becomes monotonically decreasing; the reclaim-and-respend behavior disappears.
- The selection is always a valid cut: no holes, no root collapse, no coarsening of committed siblings.
- `selected = min(residentPointCount, remaining)` can no longer truncate, because `remaining >= p` at every node. **Partial-block drawing disappears from the hierarchical path entirely**, which removes the need for any sub-node thinning there.
- `nextRefined` has nothing to undo.

Also: pass the already-computed `RenderSelectionNodeState` into the recursive call instead of re-invoking `lookup`. That halves `nodePayload` calls, removes the residency race, and cuts selection time.

And decouple the request gate. The simplest correct change is to **delete the outer `if (estimatedChildPoints <= remaining)` gate entirely** and always call `requestNode` on visible children, because `requestNode` already self-limits against `requestRemaining` (`RenderSelection.cpp:415-427`). Then have `PointFrameCoordinator` pass an independent `requestPointBudget` instead of `layerRemaining` (2M is a reasonable start; Phase 8 formalizes it). `RenderSelectionParameters` already documents the field as independent, so only the call site and the gate change.

Note that requests are still only issued below a node that *wants* refinement, so this does not prefetch beyond the desired LOD.

### Scope boundaries

- `planLayerPointBudgets` is **not** touched here. Cross-layer fairness stays exactly as it is until Phase 10; this phase fixes behavior *within* a layer's allocated budget.
- Root retention is already handled: `PointCloudScene` unconditionally pins the root (`PointCloudScene.cpp:1272-1276`), so changing the request set cannot evict a root payload.

Honest cost: reservation slightly under-utilizes the budget, because it reserves `p_i` for children that may end up cheaper. That is the price of removing order-dependent collapse, and Phase 10 replaces the mechanism wholesale.

Verification note: until Phase 4 lands, a mid-size document may take the full-detail path and bypass this code entirely. `RenderSelectionTests.cpp` is the primary verification; to observe the change in the running app before Phase 4, use a document above the full-detail threshold or lower `--gpu-cache-mb`.

### Acceptance criteria

- Exhausting the point budget never reduces a layer to root-only, and never discards an already-selected sibling subtree.
- A non-resident child still keeps its parent selected, affecting only that subtree.
- No selected node is ever drawn with fewer than its full resident point count.
- `remaining` decreases monotonically across a traversal.
- A subtree that cannot be drawn this frame can still be requested, bounded by `requestPointBudget`.
- Selected points never exceed the frame point budget.
- `RenderSelectionTests.cpp` is updated; expect meaningful churn in the 576 lines there.

## Phase 3: Adaptive point size

The cheapest large perceptual win, and it compensates for sample-quality problems that would otherwise require a cache rebuild.

```glsl
gl_PointSize = camera.pointSize;   // shaders/points.vert:79
```

is fed by a single fixed `pointSizePixels_` (default 2) for every block regardless of level (`RenderViewportWidget.cpp:1949`). Interior nodes are **deterministic reservoir samples** of their subtree (`ParentHierarchyBuilder.cpp:78-88`) — uniform random, not spatially stratified — so roughly a third of the area they cover has no sample within one mean spacing. Drawn at a fixed 2 px, that reads as holes and noise.

`pointSize` is already a per-block uniform (`BlockUniform`, offset 80, `PointCloudRenderer.h:47`) and there is one block per node, so no new plumbing is needed.

```
spacingPixels  = geometricError * focalPixels / distance      // == projectedErrorPixels()
pointSizePixels = clamp(coverageFactor * spacingPixels * userScale, 1, maximumPointSizePixels)
```

`coverageFactor` accounts for sampling clumping: ~2.0 for reservoir-sampled interior nodes, ~1.2 once Phase 9 makes them spatially stratified, ~1.0 for true leaves. It keys off `PointCloudNode::leaf`, which is why Phase 1b must land first — a detail-capped level that falsely reports itself a leaf would get 1.0 while actually carrying 16,384 reservoir samples, producing exactly the holes this phase exists to remove. The user's point-size setting becomes `userScale`, a bias rather than an absolute.

Flat blocks have no node and no geometric error, so they need their own spacing estimate. Derive it from the block's own tight bounds and the number of points actually drawn, which is the truncated count, not the stored count:

```
spacing = tightBounds.maximumExtent() / sqrt(drawnPointCount)
```

That makes a heavily truncated flat block draw proportionally larger sprites, which is what keeps a partially drawn 200 m cell from reading as a sparse stripe before Phase 9a reorders it.

The formula is exactly `projectedErrorPixels()`, which yields a useful property: refinement triggers above 3 px, so in steady state sprites stay small; **they grow precisely when refinement is blocked**, which is exactly when holes would otherwise appear.

Ship the geometric-error fix with this phase, because it is the same quantity. Error is currently `extent / sqrt(pointsPerLeaf) / 2^level` (`LocalPointPageSource.cpp:313`), a pure function of level. **No format change is required**: `tightBounds` and `pointCount` are already persisted per page, so a density-aware error is derivable at load time. Persist a measured parent-to-child residual only if the derived value proves insufficient, and only in the Phase 9 rebuild.

### Things that must be checked, not assumed

- `gl_PointSize` above 1 requires the Vulkan `largePoints` feature and is device-limited. The existing 1-8 range is exercised today; do not exceed `maximumPointSizePixels = 8` without querying the device limit.
- The pick radius uses the global `pointSizePixels_` (`RenderViewportWidget.cpp:2137`). With per-node sizes it must use the largest size actually drawn, or a click on a large sprite will miss.
- Eye-dome lighting shades from the depth buffer. Verify EDL still reads correctly with variable sprite sizes.

### Acceptance criteria

- A node selected at its refine threshold renders with no visible holes at that distance.
- Point size falls to 1 px as spacing drops below a pixel near the leaves.
- Picking a point still succeeds at every drawn sprite size.
- Geometric error varies with measured node density, not only with level.

## Phase 4: Remove automatic whole-document full detail

Pure deletion. Do it after Phases 1-3 so the LOD path is already correct when it becomes the only path.

The current policy pins every leaf (`FullDetailController.cpp:108`), uploads and protects every leaf block *before* testing visibility (`FullDetailController.cpp:206-215`), and collapses every source to its root when any one page is missing (`FullDetailController.cpp:228-255`).

Note the real threshold. A 113M-point document is 1.81 GB at 16 bytes per point and needs about 2.07 GB of GPU budget to pass `fullDetailResidencyFits()`, so at the 512 MiB default it never qualifies. The trap fires at roughly **29M points** with default settings. It is a mid-size-document bug, more common than it first looks, but it is *not* the 100M+ mechanism. Phases 2, 3 and 6 are what fix 100M+.

Remove:

- `src/renderer/planning/FullDetailController.cpp` and `.h`, and their `src/CMakeLists.txt` entries.
- `FullDetailResidencyCandidate` and `fullDetailResidencyFits()` from `RenderSelection`.
- Full-detail configuration, progress, status and planning APIs from `PointFrameCoordinator`, and the branch in `buildPlan()`.
- Full-detail plan refresh, progress publication and cleanup from `RenderViewportWidget`.
- The condition suppressing `AdaptivePointBudget` samples during full detail (`RenderViewportWidget.cpp:1276`).
- The full-detail override in `framePointBudget()` (`RenderViewportWidget.cpp:1834`).
- `FullDetailWarming` and its session, overlay, diagnostic, qualification and telemetry fields.
- `PointCloudScene::setPinnedNodes()`. Its only callers are `FullDetailController.cpp:108` and `:365`, reached through the descriptor built at `PointFrameCoordinator.cpp:75`. Once the controller is gone the API is dead; either remove it or keep it with a comment saying it is reserved for Phase 13 export. Do not leave it undocumented and unused.
- `FullDetailControllerTests.cpp`, with `tests/CMakeLists.txt` updated.

Removing the pin set does not weaken root retention: `PointCloudScene` pins the root unconditionally and independently (`PointCloudScene.cpp:1272-1276`).

Do **not** remove the settled-flat override at `RenderViewportWidget.cpp:1836` here. It is a real feature that requirement 3 depends on; Phase 6 replaces it with something bounded.

Fix display readiness in the same change. It blocks until full detail is *active* (`RenderViewportWidget.cpp:1585`), so a layer reports "loading" until every point is decoded **and** GPU-resident. Readiness should mean a stable coarse representation is GPU-resident. An off-screen completed layer is display-ready without GPU residency, because it deliberately has no draw work.

Make `buildPlan` degrade rather than throw. `hierarchyError()` currently propagates a `throw` out of frame planning (`PointFrameCoordinator.cpp:440`), so one bad source takes down the frame. Drop the layer and report it.

Keep `PointCloudDataSource::fullDetailInfo()`; it costs nothing unused and may serve export later.

### Acceptance criteria

- No document can automatically request or pin its complete leaf set.
- Adaptive point-budget updates run during all ordinary rendered frames.
- `framePointBudget()` cannot return total source points.
- Load completion no longer depends on whole-document GPU residency.
- A hierarchy error disables one layer, not the frame.

## Phase 5: Measurement harness

This phase exists because the rest of the plan repeatedly defers decisions to qualification data that **cannot currently be produced**. Without it, "defer until profiling shows X" is a promise that can never be redeemed.

What exists: `QualificationReporter` records `RenderMetrics`, writes a report, and is driven by `--qualification-report` / `--qualification-exit` behind `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI`.

What does not exist, and must be built:

- **A deterministic camera-path driver.** There is no replay, flythrough or scripted-camera facility anywhere in the tree. Needed: a path definition (keyframes plus a fixed frame count), a warm-up interval, and a mode that advances the camera on a frame counter rather than wall-clock so runs are comparable across machines.
- **Per-frame metrics retention.** `QualificationReporter` keeps only `lastRenderMetrics_` and a vector of frame times. It must retain the full per-frame series to evaluate p95 and saturation criteria.
- **The assertion layer** that turns recorded criteria into pass/fail.

### Portable synthetic qualification

Independent of the above and cheap, because it is only tests: build a deterministic in-memory forest of at least six sources and thousands of nodes without allocating the represented points, and assert planning invariants and bounded planner operation counts rather than wall-clock timing.

Cover: six overlapping sources; differing bounds, densities and depths; phantom children; missing CPU pages and missing GPU blocks; camera paths crossing source seams; tight point and draw budgets; reordered document layers.

### Real six-file qualification

Replay the six LAS files (~113M points) along a fixed path that **must** include a zoom-to-region segment and a stationary dwell, so requirement 3 is exercised.

Pass criteria, stated relative to viewport pixels where point counts are involved:

- All six in-frustum sources remain represented.
- No automatic full-detail warming, all-leaf request, or all-leaf pin set occurs.
- While interacting, submitted points settle at roughly 1-4x the viewport pixel count and remain adaptive. At the 1280x800 qualification window that is ~1.0-4.1M.
- Point uploads never exceed 48 MiB per frame.
- GPU and decoded CPU caches remain inside their configured ceilings.
- **No roots-only frame occurs for any layer during ordinary navigation or refinement, from any cause.**
- LOD changes are localized to affected subtrees.
- Warm p95 frame time targets 16.7 ms while interacting on the reference machine.
- **Selection time stays under 2 ms.** Recorded today but never bounded, and Phase 10 makes it a real risk.
- A stationary camera framing a small region reaches full source density for that region within a bounded number of frames.
- Continuous eviction or repeated re-requesting of the same visible working set is a failure even inside the memory ceiling.

Draw calls are deliberately **not** capped in these criteria; see Phase 8. Record source points, requested points, selected points, submitted points, draw calls, visible and covered layers, decode requests, uploaded bytes, protected bytes, resident bytes, evictions, culling counts by cause, selection time, command-recording time, and GPU frame time for every frame.

## Phase 6: Idle progressive refinement

Replaces the settled-flat override rather than deleting it, and delivers requirement 3 for framings wider than the interactive budget.

While the camera moves, hold the interactive budget. Once it is still, grow the frame point budget in bounded increments toward the usable residency ceiling (~29.8M points), refining frustum contents until everything visible is at leaf detail or the ceiling is reached. `buildPlan` already clamps `framePointBudget` to `transitionSafeCapacity`, so that ceiling is enforced without new code.

### Interactions that must be handled explicitly

- **The adaptive controller will fight the growth.** `AdaptivePointBudget::update()` cuts the budget 10% after three samples above 18.34 ms. A stationary 20M-point frame may take 40 ms and that is *fine* when nothing is moving. Idle frames need a separate, looser frame-time target, or the controller's downward pressure must be suspended while idle.
- **Growth must be faster than the controller's ramp.** `current/20` every six fast samples is far too slow for convergence; use a dedicated idle ramp.
- **Snap down on the first input event, before planning.** `framePointBudget()` already consults `input_.hasMovement() || dragMode_ != DragMode::None`, so the interactive budget is restored on the same frame movement starts. Verify that ordering holds.
- **Requests must lead draws.** This depends on the Phase 2 request-budget decoupling; without it, growth can only request what it can already draw and convergence stalls one level per frame.
- **The draw budget must not block convergence.** A fully refined idle frame is ~910 draws. See Phase 8.

### Acceptance criteria

- Camera motion restores the interactive budget on the same frame.
- Idle growth is bounded per frame and never exceeds `transitionSafeCapacity`.
- Idle growth never evicts a block the current draw plan needs.
- A stationary camera framing a small region reaches full source density within a bounded frame count.
- Frame-time feedback does not claw back idle growth while the camera is still.

## Phase 7: Point culling

Ordering rule: **cull before allocating budget, never after.** The point budget must count points that will produce pixels. Frustum-culled blocks are already skipped before allocation, which is right. Classification-filtered points are not, which is wrong.

Already present and to be kept: source-bounds culling (`SceneVisibilityIndex`), node-bounds culling (the `VisibilityTest` into `RenderSelection::select()`), flat-block culling, pick-volume culling (`intersectsScreenPickVolume`).

### 7.1 Front-to-back draw order

Depth test and write are on with `Less` (`PointCloudRenderer.cpp:361-363`) and `points.frag` neither discards nor writes depth, so early-Z engages cleanly. Hierarchical draws are submitted in traversal order (`PointFrameCoordinator.cpp:511`); the all-flat path uses a stable, camera-independent hash order.

The key distinction: **stability is about which blocks are selected, not the order they are submitted in.** Re-sorting the selected set front-to-back each frame changes no membership and costs one sort of a few hundred elements.

Be honest about the size of the win. `points.frag` is a `mix` and one texture sample, and at 2 px sprites overdraw is roughly 2x, so the saving is a fraction of a small cost. It grows with Phase 3's adaptive sizes (an 8 px sprite is 16x the fragments) and with scene depth complexity. Do it because it is ten lines and cannot regress correctness, then **measure it with the Phase 5 harness** rather than assuming it mattered. Note it interacts with draw ordering for pipeline and uniform-update locality; check `uniformUpdateOperations` does not regress.

### 7.2 Classification-aware block accounting

Filtering is per-point in the vertex shader (`shaders/points.vert:84-87`). A rejected point still pays vertex fetch, shader invocation and primitive assembly, and still occupies its slot in the point budget.

A presence mask alone is weaker than it first appears: it only rejects blocks that are **100%** filtered out, and a 32,768-point Morton chunk usually contains several classes. The win is real but data-dependent — large when the retained classes are rare and spatially localized, near zero for ubiquitous classes such as ground.

Store a small **per-class histogram** instead, which does both jobs:

- Reject a block when every retained class has a zero count.
- Give the planner an accurate *drawn* point count for a partially filtered block, so the budget means drawn points rather than submitted points.

`PointClassificationFilter` is already a 256-bit mask over `pointClassificationCount = 256` (`PointClassificationFilter.h:11`), so the presence test is a mask intersection against the same type. Store nonzero `(class, count)` pairs only; real data has under a dozen. **No format change is required**: compute it where the block is constructed — `BlockPartitioner::seal` for flat blocks, page decode for hierarchical ones — at a cost of one pass over at most 65,536 points.

Accounting detail: the histogram adds a few dozen bytes to `PointBlock`, which is a CPU-side structure. It must be included in decoded-byte accounting or explicitly excluded with a comment, and it must not change GPU residency bytes, which are derived from `points.size() * sizeof(GpuPoint)`.

### 7.3 Sub-pixel node culling

`RenderSelection` skips a node only on the frustum test. A node whose projected bounds cover under about one pixel is still drawn and still consumes budget. Add an explicit minimum projected-size test beside the existing `projectedErrorPixels` computation; same inputs, no new metadata.

Two guards:

- A source's root is exempt, so this can never silently drop a whole source and break the coverage invariant.
- It depends on Phase 1. Before the node-existence contract, phantom children report source-sized bounds and can never be sub-pixel, so the test would be inconsistent.

### 7.4 Occlusion culling: consider, do not build yet

For street-level views of a terrestrial scan most points are behind a facade, and neither LOD nor frustum culling can see that. The realistic option under QRhi is a hierarchical-Z pyramid from the previous frame's depth, tested against node bounds.

Costs: a depth downsample chain every frame; one to two frames of latency, which shows as popping when the camera swings; and either compute-shader support on every backend or a readback stall.

Gate it on Phase 5 data showing overdraw is actually the bottleneck, and apply 7.1 first. If it becomes necessary, test node bounds against a coarse pyramid. Per-point occlusion culling for point clouds is a research topic, not a feature.

### 7.5 Explicitly rejected

- **Back-face or normal-cone culling.** `GpuPoint` carries no normals, and adding them costs 4-8 bytes against a 16-byte record: a 25-50% residency hit for a technique that assumes oriented surfaces the data does not carry.
- **Per-point CPU culling.** At 1-4M points per frame any per-point CPU pass costs more than the GPU work it saves. Culling belongs at block and node granularity.

### Culling metrics

Add to `RenderMetrics`: blocks and nodes rejected by frustum, by classification, and by sub-pixel size; points eliminated by each; whether draws were submitted in depth order.

## Phase 8: Explicit frame-work budgets and telemetry

Replace the single frame point limit with an explicit bundle:

```cpp
struct PointFrameBudgets {
    std::uint64_t points = 1'000'000;
    std::uint32_t draws = 256;          // interactive; see below
    std::uint32_t nodeRequests = 128;
    std::uint64_t requestPoints = 2'000'000;
    std::uint64_t uploadBytes = 48ULL * 1024ULL * 1024ULL;
};
```

- `points`: maximum points selected this frame; driven by `AdaptivePointBudget`.
- `draws`: maximum point-cloud draw calls.
- `nodeRequests`: maximum new hierarchy pages requested in one frame.
- `requestPoints`: decode/upload prefetch working set (already introduced in Phase 2).
- `uploadBytes`: GPU upload traffic admitted in one frame.
- GPU residency bytes and decoded CPU bytes remain long-lived cache ceilings, not frame-detail targets.

Smaller than it looks: the 48 MiB upload budget already exists (`UploadScheduler.h:55`), telemetry is already rich (`RenderMetrics.h`), and `requestPoints` lands in Phase 2. The genuinely new items are `draws` and `nodeRequests`.

**The draw budget must be idle-aware.** With one block per node, an interactive 1M-point frame is 30-61 draws, so a cap in the low hundreds never binds there. But a fully refined idle frame is ~910 draws, and a fixed 192 cap would silently prevent Phase 6 from ever converging. Define the draw budget as a function of the current point budget rather than a constant — for example `draws = clamp(points / 4096, 256, 1200)` — and treat it as frame-time protection, not a tuning lever. This is also the case that makes Phase 12 relevant: draw-call overhead matters when idle, not when interacting.

On ordering: **no draw cap exists today**, so Phase 6 is not blocked by this phase running after it. The constraint runs the other way — do not introduce a fixed draw cap at any point before this phase's idle-aware formula exists, or Phase 6 will regress silently and the qualification criteria will still pass, because they deliberately do not cap draw calls.

Only blocks needed for the current draw plan and admitted parent-to-child transitions are protected from eviction. Invisible pages and speculative refinements remain evictable.

Extend metrics with configured limits, per-limit saturation flags, requested versus admitted node counts and estimated points, protected GPU bytes and blocks, and parent-fallback and transition counts.

## Phase 9: Thinning and point ordering

### The invariant

**Interior nodes are thinned. Leaf nodes are delivered complete, always.**

After Phase 2 this holds at runtime for free, because reservation removes partial-block drawing from the hierarchical path. What remains is build-time sample *quality*, plus the flat path.

### 9a. Flat-block progressive ordering — no rebuild, independent, pullable forward

This is the worst thinning in the system today and it is what users see while a large file is still indexing.

`targetPointsPerCell = 262144` and `cellsPerAxis = cbrt(N / 262144)` (`BlockPartitioner.cpp:41-47`) give a 20M-point file 5 cells per axis: **200 m cells** on a 1 km tile, capped at 65,536 points per block (`PointBlock.h:18`). `planFlatFrame` then calls `planStableFlatBlocks` (`FramePlanner.cpp:58`), which hands nearly every block a *fractional* cumulative allocation, and block points are in arrival order. The drawn prefix is a scan stripe across a 200 m cell.

Flat sources have no hierarchy, so truncation is their only way to reduce points; the fix is to make prefixes spatially progressive rather than to stop truncating. `BlockPartitioner::seal` is the hook — it receives a mutable `std::shared_ptr<PointBlock>&` before publishing it as `PointBlockPtr` (const), so one sort of at most 65,536 points there makes every downstream prefix valid without touching the flat planner.

Two correctness details: reorder `points` and `attributes` in lockstep, and use a deterministic tie-break, because per-point ids derived from block offsets must stay stable across sessions for a source read in the same order.

### 9b. The single cache-rebuild milestone

Interior samples are **deterministic reservoir samples** (`ParentHierarchyBuilder::sampleChildren`, `ParentHierarchyBuilder.cpp:78-88`) — uniform random, not spatially stratified. `SpatialRootPreview`'s grid-occupancy sampler is used only for the transient early root during a build; the committed root is rebuilt from `sampleChildren` too (`LocalPointIndexBuilder.cpp:486-505`). So **no committed level uses stratified sampling**.

This information is destroyed at build time and cannot be recovered at decode. Improving it requires rebuilding the page store, which means bumping `localPointPageBuildRevision` in `src/import/local/LocalPointPageFormat.h` and every user re-indexing their data.

Therefore: **there is exactly one cache-rebuilding milestone. Batch every build-format change into it and schedule it deliberately.** Candidates:

1. Spatially stratified interior sampling (grid occupancy, generalizing `SpatialRootPreview` to every interior level and to the final root). Fix its input-order dependence at the same time: it is currently "first arrival in the cell wins"; tie-break on distance to cell centre instead, which is order-independent and yields a mild centroidal quality for free.
2. Progressive point ordering within pages, only if picking or export need a canonical progressive order. Phase 2 removes the rendering need for it.
3. A persisted parent-to-child sampling residual, only if the Phase 3 load-time derived geometric error proves insufficient.

Payoff to state in the milestone's justification: stratified samples let `coverageFactor` in Phase 3 drop from ~2.0 to ~1.2, so the same coverage is achieved with smaller, sharper sprites.

Explicitly not doing: true blue-noise or Poisson-disk decimation (needs a k-d neighbourhood pass, and a prefix of a blue-noise set is not blue noise without a full radius hierarchy), and curvature or feature-preserving decimation (makes LOD transitions pop, because the retained set changes character between levels).

### Density-proportional versus density-equalizing

| | Reservoir / stride (density-proportional) | Grid occupancy (density-equalizing) |
| --- | --- | --- |
| Screen density | Uneven, mirrors scan density | Uniform |
| Budget use | Poor. An overscanned facade at 5000 points/m^2 takes 100x the budget of 50 points/m^2 ground for no extra screen information | Near-optimal |
| Holes | ~1/3 of cells empty at equal sample count | None |
| Density cue | Preserved | Lost |
| Build cost | Reservoir: free. Stride: free | One hash-set pass per node |

Choose density-equalizing for interior levels. The users navigate and measure; scan density is an analysis signal that belongs to an explicit analysis mode. Uniform screen density is what an octree LOD level is supposed to deliver.

### Acceptance criteria

- Interior-node samples cover all occupied cells of the node when sample count allows.
- A leaf node is never resampled and never partially drawn.
- Repeated builds produce byte-identical output.
- Input iteration order does not affect the sample set.
- Flat-block prefixes at 1/8, 1/4 and 1/2 cover the block's full extent.
- Format-version round-trip and rejection tests accompany the revision bump.

## Phase 10: Global hierarchical LOD planner

Replace per-layer allocation plus independent depth-first traversal with one planner over a forest of every visible hierarchical source root.

1. Frustum-cull source bounds and roots.
2. Reserve deterministic coarse coverage for every visible source.
3. Generate eligible refinement candidates across all sources, excluding nonexistent nodes per Phase 1.
4. Rank by projected error reduction against incremental point and draw cost. Costs are exact (16,384 interior, 32,768 leaf), so the cost model needs no estimation heuristics.
5. Refine globally until a budget binds, using Phase 2's reservation semantics so exhaustion never coarsens committed work.
6. Request only the highest-priority missing children within the request budgets.
7. Keep a parent selected until every visible replacement child is decoded and GPU-resident.
8. Commit transitions per subtree, so ready regions refine without waiting on unrelated ones.
9. Preserve refine/coarsen hysteresis across frames, keyed globally by `(layerId, nodeId)`.
10. Use `(layerId, nodeId)` as the final tie-break so document order cannot change the result.

Note that steps 2 and 5 are the same mechanism at two scales: reserving each source's coverage before spending on detail is exactly Phase 2's sibling reservation, one level up. Implement it once.

### Prefer a pixel-derived target over a distributed budget

Derive each node's sample count from its projected pixel area, targeting about one point per pixel per visible surface, and let the global budget be a safety cap that normally does not bind. This matters concretely: at 1M points shared across six sources, each source gets ~10 nodes and barely passes level 1. A pixel-derived target gives budget only to sources actually covering pixels.

Three benefits: the coverage invariant falls out instead of needing a reservation rule; the budget stops being a per-machine tuning number; and the mixed-source problem below disappears by construction.

### The mixed-source hole to close

`planLayerPointBudgets` is what currently splits the budget between flat and hierarchical sources, and it is being removed. Flat sources may keep their stable block planner initially, but the plan must state how one budget is shared: either admit flat block groups as pseudo-node candidates in the same queue with a projected error, or reserve an explicit share. The pixel-derived formulation resolves it and is preferred.

### Tests

- Every visible source gets root coverage before any source gets extra detail.
- A near high-error node beats a distant low-error node across different files.
- A selected subtree retains its parent until all visible replacement children are resident.
- A transition in one source does not alter stable draw nodes in unrelated sources.
- Reversed and shuffled source order produces an identical selected-node set.
- Point, draw, request-count and request-point limits are never exceeded.
- Invisible and nonexistent children are neither requested nor required for a transition.
- Hysteresis prevents oscillation around a single error boundary.
- Budget exhaustion never coarsens an already-committed subtree.
- Selection time stays within the Phase 5 bound on the six-source fixture.

## Phase 11: Measurement precision

Correct a common misreading first: grid-occupancy and reservoir sampling both select **actual source points**, they do not average. A measurement snapped to an interior-node sample therefore already lands on real data. What coarse LOD costs is which point, and — more seriously — precision.

Block coordinates are 16-bit quantized with `scale = tightExtent / 65535` (`PointBlock.h:19`, `PointBlock.cpp:41`). Because a node's extent roughly halves per level, **quantization precision is LOD-dependent**: on a 1 km source that is ~15 mm at the root versus ~0.9 mm at a level-4 leaf. For an inspection tool that is the real accuracy problem, and it is invisible to the user.

`PointPicker::pick()` takes `const std::vector<BlockDraw> &draws` (`PointPicker.h:49`), so picking runs against the drawn set at whatever precision that level carries.

Scope:

- Report the effective precision of the current measurement, derived from the picked block's `scale`. Cheap, and it makes the problem visible rather than silent.
- Resolve the pick against leaf data: identify the leaf whose tight bounds contain the pick ray's hit, ensure its payload is decoded, and run a CPU ray-versus-point search in that payload. This is a different path from `PointPicker` and is inherently asynchronous when the leaf is not resident, so the interaction must tolerate a short delay.

This is the smallest useful instance of ROI-scoped exact detail and a good proving ground for it.

## Phase 12: GPU buffer arenas

Profile first, with Phase 5 data. The relevant case is the **idle** frame at ~910 draws and one immutable buffer per page, not the interactive frame at 30-61 draws.

- Allocate a small number of large point buffers.
- Suballocate aligned byte ranges to resident blocks.
- Store arena identity, byte offset, byte size, point count and LRU state per residency record.
- Return freed ranges to an allocator on eviction.
- Upload directly into arena ranges through a reusable staging path.
- Preserve byte-accurate residency and eviction accounting.
- Compact or retire empty arenas without invalidating active frame resources.

`PointCloudRenderer` already parameterizes the vertex-buffer offset (`QRhiCommandBuffer::VertexInput(draws[i].buffer, 0)`, `PointCloudRenderer.cpp:197`), so binding an arena plus a block offset is a small renderer change.

Add grouped or indirect drawing only if QRhi and all supported backends expose a clean implementation and telemetry still shows command recording or submission as a bottleneck. Multi-draw is not a prerequisite for anything else in this plan.

### Tests

- Arena allocation never exceeds the residency ceiling.
- Eviction frees the exact suballocated range and updates counts.
- Protected ranges cannot be evicted during an active transition.
- Reused free ranges retain required alignment.
- Device or resource reset releases every arena and residency record.
- Rendering, picking, color modes and classification filters stay correct at non-zero buffer offsets.

## Phase 13: Explicit exact-detail operations

Phase 6 covers interactive zoom and Phase 11 covers measurement, so what remains is narrow:

- Export reading directly from source or page storage.
- A static high-quality render with explicit progress and cancellation.

Such an operation must never start automatically because data happens to fit, must not request data outside its declared scope, must use bounded decode and upload work per frame, must be cancelled or replanned when its document revision or ROI becomes invalid, must preserve the ordinary LOD view while pages are prepared, and must protect only its active scoped working set.

If a whole-document exact mode is ever required, it must be explicitly selected, warn about its point and memory cost, and be treated as a static operation rather than the interactive renderer.

## Summary

| # | Phase | Size | Requires | Rebuild | Primary tests |
| --- | --- | --- | --- | --- | --- |
| 1 | Node metadata contract (existence, detail cap) | S | — | no | `RenderSelectionTests`, local-page source tests |
| 2 | Non-collapsing node selection | M | 1 | no | `RenderSelectionTests` |
| 3 | Adaptive point size + derived geometric error | S | 1b | no | `RenderSelectionTests`, renderer smoke |
| 4 | Remove automatic full detail | M | — | no | delete `FullDetailControllerTests`, `PointFrameCoordinatorTests` |
| 5 | Measurement harness | M | — | no | synthetic forest fixture, six-file replay |
| 6 | Idle progressive refinement | M | 2 | no | `PointFrameCoordinatorTests`, replay dwell segment |
| 7.1 | Front-to-back draw order | S | — | no | measured via 5 |
| 7.2 | Classification-aware block accounting | M | — | no | `BlockPartitioner`, page-decode tests |
| 7.3 | Sub-pixel node culling | S | 1a | no | `RenderSelectionTests` |
| 8 | Budgets and telemetry | S | — | no | `PointFrameCoordinatorTests` |
| 9a | Flat-block progressive ordering | S | — | no | `BlockPartitioner` ordering tests |
| 9b | Cache-rebuild milestone | L | — | **yes** | format round-trip and rejection tests |
| 10 | Global forest planner | L | 1, 2 | no | new planner tests, synthetic forest |
| 11 | Measurement precision | M | — | no | picker tests |
| 12 | GPU arenas | L | 5 | no | upload/residency tests |
| 13 | Export / static exact detail | M | — | no | export tests |

Phases 1, 2, 3 and 6 together deliver all three requirements, and none of them needs the planner redesign or a cache rebuild.

## Implementation notes

**What is reversible.** Every phase except 9b is a code change that can be reverted. 9b bumps `localPointPageBuildRevision` and forces every user to re-index; it is the only one-way door in this plan and should be scheduled deliberately, with every build-format change batched into it.

**What deliberately does not change before Phase 10.** `planLayerPointBudgets` and the flat block planner keep their current behavior throughout Phases 1-9. `RenderSelection` stays per-layer, keyed by `hierarchySelections_`. Phases 1 and 2 fix behavior *inside* a layer's allocated budget; cross-layer fairness is Phase 10's problem. An implementer who finds themselves editing `planLayerPointBudgets` before Phase 10 has drifted out of scope.

**Order flexibility.** 9a is independent of everything and can be pulled forward at any time; it is the most visible improvement per line changed for anyone watching a large file index. 7.1, 7.2, 8 and 11 have no hard predecessors and can be resequenced freely. 1 before 2 before 6 is a real chain and must be respected. 1b before 3 is a real chain.

**Phase 5 should not slip.** 7.1, 7.4 and 12 are all explicitly gated on data only the harness can produce. Without it those three become guesses, and the plan's repeated instruction to "measure first" is unenforceable.

**Definition of done for a phase.** Its acceptance criteria are covered by tests that run in CI, the metrics it names are recorded, and — from Phase 5 onward — the six-file replay still passes every criterion it passed before. A phase that improves one metric while silently regressing another is not done.
