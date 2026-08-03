# Multi-Point-Cloud Scene Implementation Plan

## Goal

Allow users to load multiple point-cloud files into one scene. When a scene
already contains a cloud, opening another file prompts the user to either add
it to the current scene or open it as a new scene.

The design must retain an independent identity, visibility state, and colour
mode for every cloud so future layer controls do not require restructuring the
import or renderer paths.

## User Behaviour

- Opening the first file loads it as a new scene without a prompt. It streams
  in progressively from `sceneReady`, exactly as today.
- The file picker accepts multiple files. A selection is processed one file at
  a time: the first uses the chosen mode and every remaining file is added to
  the resulting document. The next import waits for the prior import and its
  first rendered frame. Cancellation or failure stops the remaining queue.
- Opening another file shows an application-modal dialog with `Add to Scene`,
  `Open New Scene`, and `Cancel`.
- `Add to Scene` keeps existing clouds visible and preserves the current
  camera view. The new cloud is added progressively as its scene shell becomes
  available.
- `Open New Scene` keeps the current scene visible and rendering until the new
  cloud finishes loading, then swaps it in and frames it. If the import fails
  or is cancelled, the current scene was never touched — nothing is restored
  and the camera does not move.
- The shared Open action is disabled from the beginning of a load until the
  first rendered frame, with load completion, failure, and cancellation as
  re-enable fallbacks (`FirstFrameReady` only fires once resident points
  exist, so a load yielding none must not leave Open disabled). The existing
  Cancel action remains available.
- The existing colour controls apply to every visible cloud. Their choices are
  restricted to modes supported by all visible clouds. Adding a cloud never
  coerces the colour modes of existing clouds: when visible clouds' modes
  diverge, or the active mode falls outside the supported intersection, the
  combos show a blank placeholder state; a user selection then unifies all
  visible clouds to that mode. The intersection can never be empty because the
  X/Y/Z sources are unconditionally available
  (`availablePointColorSources`, `src/renderer/PointColorPolicy.h:44-46`).

## Scene Model

Create a pure scene-library model above `PointCloudScene`:

```cpp
using PointCloudLayerId = std::uint64_t;

struct PointCloudLayer {
    PointCloudLayerId id;
    PointCloudScenePtr scene;
    bool visible = true;
    PointColorMode colorMode;
};
```

`PointCloudDocument` owns ordered layers and exposes:

- `addLayer(PointCloudScenePtr)` returning a stable ID;
- removal and lookup by ID;
- visibility and colour-mode updates by ID;
- a snapshot of layers for rendering;
- a monotonic document revision for structural or layer-state changes;
- aggregate bounds and point counts for visible layers.

Each layer receives `defaultPointColorMode(scene->metadata())` when added.
The document is mutated on the UI thread. `PointCloudScene` continues to own
its streaming blocks and remains safe for the loader worker thread.

## Renderer Changes

- Replace the single-scene `RenderViewport` contract with a document contract.
  The viewport receives a `PointCloudDocumentPtr` and renders its visible
  layers. Layer order carries no rendering meaning: points are opaque and
  depth-tested, so the draw list flattens all visible layers' blocks into one
  list, culls it, and sorts it near-to-far globally — per-layer ordering would
  contradict the existing global sort
  (`RenderViewportWidget.cpp:345-354`).
- Build draw records from every visible layer. Use the owning layer's colour
  mode, bounds, and intensity range when preparing its uniforms.
- Use aggregate visible point counts for metrics, and apply the single
  adaptive point budget across the flattened, globally sorted draw list.
  Retain a single global pick-ID sequence across all submitted draws.
- Keep GPU buffers when a layer is added. Extend `UploadScheduler` to remove
  buffers whose blocks are no longer referenced by the active document, such
  as after a replacement or layer removal. Buffers are keyed by raw
  `const PointBlock *` (`UploadScheduler.h:42`), which is safe today only
  because replacement does a full `releaseResources()`; with selective
  retention, eviction must be sequenced before the last block references are
  dropped — or the map re-keyed to hold `shared_ptr`/`weak_ptr` — to prevent
  dangling-key/ABA buffer reuse.
- Track per-layer resident point counts (today only a single global
  `residentPoints_` exists) so upload progress can be attributed to a layer.
- Include the layer ID in renderer load-progress events so the application can
  associate progressive upload and first-frame completion with the active
  import.
- Reframe the camera only when installing a document that replaces another,
  or the first document. Adding a layer must not move it.

## Application Loading Flow

- Let `MainWindow` own the active `PointCloudDocument` and the pending load
  mode (`Add` or `Replace`). Keep direct `loadPointCloud(path)` calls as
  replace-scene behavior for compatibility and add an explicit mode overload
  for tests.
- Extract a small testable dialog component rather than embedding an
  untestable static `QMessageBox` call. It returns `Add`, `Replace`, or
  `Cancel` and has named buttons for offscreen UI tests. It must not use
  `exec()` — a blocking modal loop hangs the offscreen `QTest::qWaitFor`
  harness used by `MainWindowTests`; use `QDialog::open()` with a callback or
  an injectable decision provider, matching the existing non-blocking `show()`
  pattern (`MainWindow.cpp:468-475`).
- For an add, create and attach the new layer on `sceneReady`; on failure or
  cancellation, remove that incomplete layer by ID.
- For a replacement with an existing document, swap on success: hold the
  `sceneReady` shell as a pending scene without installing it, letting blocks
  stream into it CPU-side. On `loaded`, build the replacement document,
  install it, and reframe. On failure or cancellation, discard the pending
  scene — the old document was never touched, so there is no rollback state.
  Trade-off, accepted deliberately: during a replace the new cloud does not
  stream in progressively (the old cloud stays under the overlay until the
  swap); in exchange the rollback state machine disappears entirely and GPU
  residency never doubles, because old buffers are evicted at the swap and
  new ones upload after it.
- For the first load, when no document exists, install at `sceneReady` as
  today so the first cloud still streams in progressively. On failure, current
  behavior is kept (partial scene remains, error dialog).
- Keep all existing loading-overlay progress behavior. Ignore render progress
  for layers unrelated to the active request.
- Compute colour-source choices as the intersection of sources supported by
  all visible layer metadata. Compute compatible maps from that intersection.
  Applying a selection updates every visible layer atomically from the UI's
  point of view; until then, diverged layer modes are shown as the blank
  placeholder state described under User Behaviour.

## Tests and Verification

1. Add Catch2 unit tests for document layer identity, ordering, removal,
   revision changes, visible aggregate bounds/counts, visibility state, and
   independent colour modes.
2. Add renderer-focused tests for flattening visible layers into one globally
   sorted, budgeted draw list, per-layer uniform colour state, aggregate
   metrics, global pick-ID offsets, per-layer resident counts, and
   upload-buffer retention/eviction planning — including that eviction is
   ordered before block release so no dangling buffer keys remain.
3. Extend offscreen `MainWindow` tests for the decision dialog, add versus
   replace behavior (replace keeps the old document rendering until success;
   failure or cancellation leaves it untouched and discards the pending
   scene; the first load installs at `sceneReady`; add removes the incomplete
   layer on failure or cancellation), load-action disabling with its
   re-enable fallbacks, the mixed colour-mode placeholder state, and common
   colour-mode selection across mixed metadata.
4. Run each affected Catch2 label after its stage, then run the full default
   CTest suite. Run the opt-in native Metal/GPU suite after renderer changes.

## Constraints

- The current maximum point count remains a limit per imported file, not per
  document.
- New layers are visible by default.
- A layer-list UI, visibility toggles, and per-layer colour selectors are not
  part of this change; `PointCloudDocument` is the foundation for them.
- Accepted risk, deferred to future work: `UploadScheduler` has no GPU memory
  cap, so add-mode residency grows with each layer, bounded only by the
  per-file point limit times the layer count.
