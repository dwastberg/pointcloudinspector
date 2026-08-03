# Point Cloud Inspector architecture

This document describes the current architecture and the rules for extending it.
The executable is the composition root; domain code does not discover services
through globals. CMake's target-boundary and include-boundary tests are the
executable specification of the dependency rules below.

## Target dependency graph

Arrows point from a target to its direct project dependencies. External private
dependencies are shown where they define an important boundary.

```text
pcinspector executable
  -> app_config, app_ui, development_support, import_async, import_ogr,
     import_pdal, platform, renderer

app_ui
  -> app_config, app_model, app_session, import_async, pointcloud,
     renderer_api, scene, vector, platform (private),
     Qt6::Concurrent (private), Qt6::Widgets
app_session
  -> app_config, app_model, import_async, scene, platform (private), Qt6::Core
app_config
  -> foundation, platform, renderer_api
app_model
  -> import_api, renderer_api

renderer
  -> navigation, platform, pointcloud, renderer_api, renderer_planning,
     scene, vector, Qt6::GuiPrivate (private), Qt6::Widgets
renderer_planning
  -> foundation, pointcloud, scene
renderer_api
  -> scene, Qt6::Core

import_async
  -> import_api, tasking, platform (private), Qt6::Core
import_pdal
  -> import_api, Qt6::Core (private), PDAL (private)
import_ogr
  -> vector, GDAL (private)
import_api
  -> scene

development_support -> pointcloud, scene
scene               -> foundation, pointcloud, tasking, vector
navigation          -> foundation
platform            -> foundation, operating-system APIs
pointcloud          -> foundation
tasking             -> foundation
vector              -> foundation, earcut (private)
foundation          -> standard library
```

The executable and `ApplicationBootstrap` are the only production composition
root. `app_ui` and `renderer` are siblings: application code sees only the
QRhi-free `RenderViewport` interface and must not include renderer internals.
Likewise, concrete PDAL and OGR adapters are injected into the asynchronous import
controllers; neither the controllers nor the scene model instantiate adapters.

When changing this graph, update the allow-list in
`cmake/PciArchitecture.cmake` in the same commit. The ordinary test suite runs
both `architecture_target_boundaries` and `architecture_include_boundaries`.

## Application and data flow

Startup is explicit:

```text
main
  -> parse ApplicationConfig
  -> build mutable built-in color-map catalog
  -> register embedded maps and freeze immutable snapshot
  -> ApplicationBootstrap
       -> resolve memory/cache policy
       -> create renderer viewport
       -> create one TaskScheduler and both import controllers
       -> create MainWindow and SceneSession
```

Point-cloud loading follows this path:

```text
UI command -> SceneSession -> PointCloudLoadController -> TaskScheduler worker
  -> PointCloudLoader::inspect
  -> admission using PointCloudLoadResources
  -> PointCloudLoader::load
       -> retained flat PointCloudScene, native hierarchy, or local page store
  -> queued owner-thread completion
  -> SceneSession transaction/admission
  -> SceneDocument mutation -> immutable SceneDocumentSnapshot
  -> RenderViewport::setDocument/updateDocument
```

The PDAL adapter chooses storage from explicit `PointCloudLoadOptions` and the
preflight result. Large ordinary LAS/LAZ sources use the versioned local page-store
facade; COPC uses its native hierarchy; bounded sources can remain flat. Import
errors cross the job boundary as typed `JobResult` errors. Cancellation uses
`std::stop_token` throughout the worker path.

Vector loading follows the same scheduling boundary but keeps its domain-specific
workflow: OGR inspection publishes selectable sublayers, the user selection is
resolved on the GUI thread, and selected sublayers load independently. Successful
`VectorLayerData` values are admitted to the same ordered `SceneDocument` as point
layers, including partial-success results.

Rendering is snapshot-driven:

```text
SceneDocumentSnapshot
  -> SceneSnapshotCache (membership, subscriptions, per-scene snapshots)
  -> PointFrameCoordinator / FullDetailController (portable planning)
  -> PointCloudRenderer and VectorLayerRenderer (QRhi resources and commands)
  -> RenderTelemetryAccumulator -> RenderMetrics callback -> application UI
```

One ordered, structurally immutable document snapshot drives both UI membership
and renderer membership. Point scenes may publish new immutable block/page payloads
behind that document snapshot; scene revisions and invalidation subscriptions wake
the renderer without mutating document structure.

## Ownership and shutdown

- `main` owns the `MainWindow` returned by `ApplicationBootstrap`.
- `MainWindow` owns the `RenderViewport` and `SceneSession`. Qt owns its child
  widgets and docks.
- `SceneSession` owns `ImportServices`, the current `SceneDocument`, application
  memory policy, and load transactions.
- `ImportServices` owns exactly one `TaskScheduler`, both controllers, and the
  statistics provider. Both controllers borrow that scheduler.
- `SceneDocument` owns ordered layer state and the document-wide point-memory
  budget, hierarchy residency coordinator, decoded-page cache, and hierarchy task
  scheduler. Layers share their scene/data payloads with immutable snapshots.
- `PointCloudScene` owns either flat storage or hierarchical storage. Hierarchical
  scenes register with document residency while attached and restore standalone
  residency before removal.
- `RenderViewportWidget` owns renderer passes, planning coordinators, snapshot
  subscriptions, and all QRhi resource owners. QRhi handles use the RAII wrappers in
  `renderer/rhi/RhiResource.h`.
- Immutable point blocks, decoded page payloads, scene snapshots, document
  snapshots, and the frozen color-map catalog use `shared_ptr` leases because they
  can legitimately span worker, GUI, and frame lifetimes.

Destruction is intentionally ordered. `MainWindow` destroys `SceneSession` before
the viewport because members are destroyed in reverse declaration order. The
session invalidates controller callback targets, destroys the controllers so their
jobs request cancellation, waits once for the shared scheduler, then releases the
document and its hierarchy resources. The viewport clears full-detail pins and
scene subscriptions before its QRhi teardown. Never wait for a worker while holding
a scene, cache, controller, or GUI mutex.

## Thread rules

| Context | May do | Must not do |
|---|---|---|
| GUI/Qt owner thread | Mutate `SceneSession` and `SceneDocument`; update models, docks, and viewport configuration; drain job completions | Perform PDAL/OGR scans, page-store builds, or blocking scheduler waits during normal operation |
| TaskScheduler worker | Inspect/load sources, build/reopen local stores, decode hierarchy pages, report progress through thread-safe callbacks | Touch QWidget/QObject state, inspect `QPointer`, or mutate document membership |
| Render/QRhi callback context | Reconcile immutable snapshots, plan a frame, create/use/release QRhi resources, publish metrics through callbacks | Mutate the application document or perform source import |

`SceneSession` asserts its QObject owner thread on command and completion paths.
Workers publish through `QueuedControllerTarget`: the cross-thread queue is guarded
by a standard mutex, while the controller pointer is read and invalidated only by
its Qt event-loop thread. Do not replace this with cross-thread `QPointer` reads.

`PointCloudScene`, `DecodedPageCache`, hierarchy admission, residency coordination,
and `TaskScheduler` are internally synchronized. Their snapshots and payload leases
are the preferred read interfaces. A mutex being present does not authorize adding
new cross-thread mutation to higher-level application objects.

## Cache and residency ownership

There are three distinct budgets; do not merge their policies:

1. `PointMemoryBudget` is application/document CPU point-memory policy. Flat scenes
   retain reservations for their stored vectors; hierarchy roots and decoded pages
   participate in the same ceiling.
2. `DecodedPageCache` is the document-wide CPU LRU for hierarchical payloads.
   `HierarchyResidencyCoordinator` divides available residency among active sources
   and `HierarchyDecodeAdmission` bounds concurrent/estimated decode work. Current
   frames hold payload leases, and pinned/protected nodes cannot be evicted.
3. Renderer upload/resource owners enforce the GPU point-byte budget. Frame plans
   identify uploads and protected GPU blocks; GPU eviction does not imply CPU page
   eviction.

The local `.pcipages` store is persistent indexing, not decoded residency. Its v1
format is a compatibility boundary under `import/local`; construction stages live
under `import/pdal`. Publish is atomic, concurrent builders converge on one reusable
store, and temporary build resources clean themselves up. Format changes require a
new version plus committed reopen fixtures; never silently reinterpret v1.

## Extension recipes

### Add an import adapter

1. Implement `PointCloudLoader`, `PointCloudStatisticsProvider`, or `VectorLoader`
   behind the contracts in `import_api`/`vector` without adding vendor types to the
   contract.
2. Put vendor headers and conversion code in a dedicated adapter target next to
   `import/pdal` or `import/ogr`; keep the vendor library a private dependency.
3. Inject the adapter in `ApplicationBootstrap`. Do not add adapter selection or
   construction to a controller, session, or widget.
4. Add adapter component fixtures plus controller tests using a fake contract
   implementation. Extend both architecture allow-lists for the narrow new adapter
   boundary.

### Add a scene layer kind

1. Add the immutable payload state to the closed `SceneLayer::payload` variant and
   define its visibility, bounds, ordering, and revision behavior in `SceneDocument`.
2. Extend `SceneDocumentSnapshot` once; UI models and renderer membership must read
   that shared ordered snapshot rather than maintain parallel collections.
3. Add the kind-specific inspector/editor and renderer planning value types in the
   lowest suitable target. Route commands through `SceneSession` using strong IDs.
4. Add document/snapshot tests first, then model/UI tests, portable planning tests,
   and finally native-GPU coverage. Update the dependency allow-list if a genuinely
   new edge is required.

### Add a render pass

1. Put device-independent selection, culling, or budgeting in
   `renderer_planning`; keep QRhi types out of its public headers.
2. Put pipelines, buffers, shader bindings, and commands under `renderer/rhi` and
   own every QRhi handle with the existing RAII resource types.
3. Coordinate the pass from `RenderViewportWidget` using the immutable scene/frame
   inputs. Do not expose the concrete widget or Qt private headers through
   `renderer_api`.
4. Add portable golden/unit tests for planning, renderer-internal lifetime tests,
   and native-GPU readback or smoke coverage. Update telemetry only through
   `RenderTelemetryAccumulator`.

## Verification and decisions

Use the presets documented in `README.md`. Every change runs the non-GPU suite;
ownership and concurrency changes also need ASan/UBSan and TSan, and renderer/RHI
changes need `native-gpu`. The CI workflow gates warnings, formatting, dependency
boundaries, clang-tidy, ASan/UBSan, and TSan. Same-host performance qualification is
required for changes that can affect loading, residency, frame planning, or draw
submission.

Durable project decisions are recorded in:

- `docs/adr/0001-ci-dependency-acquisition.md` — qualified CI dependency stack;
- `docs/adr/0002-cpp-formatting.md` — project formatting policy and gate.

Historical implementation plans and superseded designs live in `docs/archive/`
and `docs/superpowers/`. They are context, not current API documentation.
