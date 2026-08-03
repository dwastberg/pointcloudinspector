# Point Cloud Inspector Modernization Refactoring Guide

Date: 2026-07-31

Revised: 2026-07-31 (review amendments; see section 17)

Status: completed and archived 2026-08-02 (all 65 steps implemented)

Current architecture is documented in [`ARCHITECTURE.md`](../../../ARCHITECTURE.md).
Paths and type names in the verified-starting-point and "Why" sections below
intentionally describe the pre-refactor code; they are historical evidence, not
current API references. Final verification evidence is recorded in
[`docs/2026-08-02-modernization-closeout.md`](../../2026-08-02-modernization-closeout.md).

Audience: experienced C++23, Qt 6, and graphics engineers

## 1. Purpose

This guide turns the current codebase review into a sequence of behavior-preserving
refactors. Its goal is to make Point Cloud Inspector easier to understand, test, extend, and
profile without destabilizing the working point-cloud renderer, import pipeline, or
persistent cache.

The work is intentionally incremental. Each section ends in a buildable, tested
state and removes the superseded implementation before moving on. The guide does
not propose a renderer rewrite, a new point format, or a replacement for Qt, PDAL,
GDAL, or QRhi.

The principal outcomes are:

- application, scene, import, and renderer responsibilities have clear owners;
- CMake targets enforce the intended dependency direction;
- UI classes present state instead of orchestrating domain workflows;
- render planning and residency coordination can be tested without a QRhi device;
- import jobs have explicit options, resources, cancellation, and result types;
- duplicated compatibility paths and low-level utilities are removed;
- existing performance, cache, and rendering contracts remain measurable.

### 1.1 Scale and expectations

Be honest about what this is. The program is 65 commits across six phases. It is not
a feature program. Incidental correctness defects exposed by characterization,
sanitizers, or platform-path tests should be fixed in separately identified commits
rather than hidden inside mechanical moves. For a single engineer this is a
multi-month effort, and the repository it modifies currently has 68 commits in total.

Two consequences follow, and they shape the rest of this guide:

1. The program must be safe to abandon. Section 13 marks which phases are net
   positive at any commit boundary and which must be completed or reverted.
2. The program cannot rely on human discipline as its only safety mechanism.
   Section 5 builds the automated safety net before any code moves.

If the available time is less than the program requires, run Phase 0 through Phase 2
and stop. That subset is self-contained, independently valuable, and leaves the
codebase strictly better than it found it.

## 2. Verified starting point

At the time of this review, the repository contains approximately 31,200 lines of
production C++ and 11,800 lines of tests. The following commands succeed:

```sh
cmake --build --preset development --parallel
ctest --preset development
cmake --build --preset ci --parallel
```

The non-GPU suite contains 253 passing tests. The `ci` preset compiles project code
with warnings as errors. Native-GPU tests are opt-in and must be rerun after changes
to renderer ownership, frame planning, shaders, or GPU residency.

**Nothing runs these commands automatically.** The repository has no `.github`
directory, no CI workflow, and no pre-commit hooks. The `ci` preset is a build
configuration, not a pipeline. Every contract in section 3 is currently enforced by
an engineer remembering to type the commands above. Phase 0 fixes this first,
because the rest of this guide assumes a working gate.

The current high-level flow is:

```text
main.cpp
  -> MainWindow
      -> PointCloudLoadController / VectorLoadController
          -> PointTaskScheduler
              -> PDAL / OGR / local page-store adapters
      -> SceneDocument
          -> PointCloudScene
              -> decoded page cache and hierarchy workers
      -> RenderViewportWidget
          -> frame selection
          -> upload residency
          -> point, vector, EDL, and picking passes
```

Important strengths that should be preserved rather than redesigned are:

- `std::span`, ranges, `std::jthread`, stop tokens, and immutable shared payloads
  are already used appropriately;
- the point-cloud and vector-domain targets do not depend on PDAL, GDAL, or QRhi;
- cancellation, transactional replacement, fair scheduling, cache limits, LOD
  coverage, and display readiness have strong regression tests;
- the 16-byte `GpuPoint` layout is compact and explicitly asserted
  (`tests/unit/PointCloudContractTests.cpp:100`);
- the local point-page store has a versioned, checksummed, crash-safe contract;
- renderer and residency qualification data exists for representative local files;
- the application UI already includes only `renderer/RenderViewport.h`,
  `renderer/RenderLoadProgress.h`, and `renderer/PointColorLabels.h` — no QRhi
  header reaches `app/`, and `main.cpp` composes the widget. That separation is
  worth defending in Phase 2.

The largest structural concentrations are:

- [`src/renderer/rhi/RenderViewportWidget.cpp`](../src/renderer/rhi/RenderViewportWidget.cpp),
  approximately 3,147 lines;
- [`src/app/MainWindow.cpp`](../src/app/MainWindow.cpp), approximately 2,530 lines;
- [`src/app/PointCloudLayerPanel.cpp`](../src/app/PointCloudLayerPanel.cpp),
  approximately 2,132 lines;
- [`src/import/local/LocalPointIndexBuilder.cpp`](../src/import/local/LocalPointIndexBuilder.cpp),
  approximately 1,302 lines;
- [`src/scene/PointCloudScene.cpp`](../src/scene/PointCloudScene.cpp),
  approximately 1,158 lines.

Line count is not itself a defect. In these files it is a useful signal because the
classes also own several independent state machines and resource lifecycles.

By contrast, `main.cpp` is 324 lines. It is dense and hard to test, but it is not a
god object, and this guide does not treat it as one.

## 3. Contracts that must not change accidentally

Before beginning, treat the following as release contracts:

1. `GpuPoint` remains exactly 16 bytes and retains its current shader layout.
2. Local point-page store format and schema version 1 remain readable. A pure
   refactor must not invalidate existing cache directories.
3. Qualification JSON schemas remain unchanged unless a separately reviewed
   schema migration is approved.
4. CLI option names, defaults, exit codes, and smoke-test behavior remain stable.
5. Qt object names used by UI tests remain stable during view decomposition.
6. Point and vector imports remain cancellable and execute away from the GUI
   thread.
7. Point-cloud replacement remains transactional: failure or cancellation restores
   the previous document.
8. Batch point-cloud admission preserves input order even when previews complete
   out of order.
9. A hierarchical refinement never creates a visible coverage hole.
10. The viewport returns to event-driven sleep after work and interaction settle.
11. Point and vector rendering, picking, and EDL pass order remains unchanged.
12. Automatic coordinate color ranges continue to use document-wide point bounds.

When a proposed cleanup conflicts with one of these rules, preserve the rule and
split the behavioral change into a separate feature proposal.

Contracts 1 and 2 have automated coverage today. Contracts 9, 10, and 11 are covered
only by the opt-in GPU lane and the manual qualification workload; Phase 0 gives them
a machine-checkable gate.

## 4. Working method

Use the following loop for every numbered task in this guide.

### 4.1 Characterize before moving code

1. Identify observable behavior owned by the code being moved.
2. Add focused tests for any behavior that is currently covered only indirectly.
3. Run the focused target and the complete development suite.
4. Commit the characterization tests separately when practical.

This makes later failures attributable to the refactor instead of ambiguity in the
old behavior.

### 4.2 Move one responsibility at a time

Do not combine file moves, interface redesign, formatting, and behavior changes in
one commit. A safe extraction normally follows this order:

1. introduce the destination type with the old algorithm;
2. delegate the old owner to the new type;
3. move tests to the new boundary;
4. remove the old fields and methods;
5. clean includes and target links;
6. run verification and commit.

### 4.3 Verify each green step

For a focused change, run the smallest relevant executable first, followed by:

```sh
cmake --build --preset development --parallel
ctest --preset development
cmake --build --preset ci --parallel
git diff --check
git status --short
```

Run native-GPU and qualification checks at the checkpoints called out later rather
than after every header-only or UI-only edit.

### 4.4 Remove transitional code

A compatibility method is useful only while a known caller is being migrated. Once
all repository callers use the new API, remove the overload, old storage, tests for
the obsolete path, and comments describing it as temporary. Parallel old and new
implementations are more expensive than either design alone.

### 4.5 Do not forward-reference a type across phases

A section may only use a type that an earlier section introduces. If a proposed
interface needs a type from a later phase, either move the type earlier or change the
interface. The original draft of this guide violated this rule twice — `SceneSession`
was specified in terms of a `SceneDocumentSnapshot` introduced a phase later, and the
unified `SceneLayer` model used a `StrongId` introduced two phases later. Both are
resolved in this revision, and section 16 records the check that keeps them resolved.

## 5. Phase 0: make the safety net real

Everything after this section assumes that a mistake is caught by a machine. Today it
is not. Phase 0 is short, touches no production logic, and must land first.

### 5.1 Add continuous integration

#### Why

Section 3 lists twelve release contracts. Section 14 lists 65 commits. Nothing
currently executes a build or a test except by hand, and the recent commit history
shows coarse, infrequent commits rather than the per-step verification this guide
assumes. Without CI, "each section ends in a buildable, tested state" is an intention,
not a property.

#### How

1. Add `.github/workflows/ci.yml` running configure, build, and `ctest` with the
   `ci` preset on push and pull request. The `ci` preset already inherits
   `development` and enables warnings as errors, so building both in CI would repeat
   the same Debug suite. Keep `development` as the ordinary local preset.
2. Solve dependency acquisition explicitly. Qt 6.7 with `GuiPrivate` and
   `ShaderTools`, PDAL 2.10, and GDAL 3.4 are the hard part of this task, not the
   YAML. Prefer a pinned container image or a cached `vcpkg`/`conan` manifest so the
   build is reproducible; record the choice as an ADR.
3. Define the supported-host matrix in the same ADR. Run the full non-GPU suite on
   at least one blocking host/toolchain and configure, build, and run portable tests
   on every OS/compiler combination the README claims as supported. In particular,
   the Windows lane owns the native/non-ASCII path tests from section 6.8. If a host
   is not in CI, describe it as provisionally supported rather than silently
   promising an untested platform.
4. Keep the GPU lane out of CI. `PCINSPECTOR_ENABLE_GPU_TESTS` stays `OFF`; the GPU
   and qualification lanes remain developer-run and are gated at the phase
   checkpoints.
5. Fail the job on warnings via the existing `ci` preset.

#### Done when

A pull request that breaks the non-GPU suite or introduces a warning cannot be merged
without the failure being visible.

### 5.2 Resolve the formatting question now

#### Why

A repository-wide reformat is cheap before anything moves and expensive afterwards.
Landed mid-program it collides with every in-flight extraction and destroys `git
blame` during precisely the phases where blame is the tool you need to attribute a
regression. The original draft added format targets at commit 11 but never scheduled
the reformat itself, which is the worst of both worlds.

#### How

Choose one and record it:

- **Adopt now.** Add `.clang-format` matching the current style closely enough that
  the diff is reviewable, add `format` and `format-check` targets, and land the
  repository-wide reformat as a single commit before any other work. Add
  `format-check` to CI.
- **Defer.** Do not add `.clang-format` during this program. Revisit after
  section 14's final commit. Record this as an explicit exception to the optional
  format gate in section 16; do not claim formatting enforcement as a completion
  criterion in this case.

Do not choose a third option in which formatting arrives partway through.

#### Done when

Either the tree is uniformly formatted and CI checks it, or the guide records that
formatting is deferred and no `.clang-format` exists.

### 5.3 Add sanitizer presets

#### Why

The original draft deferred sanitizers to a tooling section and scheduled the cleanup
as the second-to-last commit of the program. That is backwards. Section 6.5 already
wants hierarchy lifecycle tests run under TSan, and Phase 4 restructures decode
callback ownership, cache leases, subscription lifetimes, and scene publication —
the exact code where data races and use-after-free live. Sanitizers that run after
the risky phase find bugs you already shipped and can no longer attribute to a commit.

#### How

1. Add `asan-ubsan` configure, build, and test presets for Clang and GCC over the
   non-GPU suite.
2. Add a `tsan` preset covering the scheduler, caches, scene publication, and
   controller concurrency tests.
3. Run both now and record the findings as a baseline. Fix or narrowly suppress
   findings in scheduler, cache, controller, and scene tests before Phase 3 starts;
   those are the suites the high-risk phases depend on. A suppression must identify
   the upstream/runtime issue and the platform on which it occurs.
4. Add the clean, supported sanitizer lanes to CI before Phase 3. If TSan is not
   supported by the primary CI toolchain, run it on a dedicated supported runner or
   make it a required developer checkpoint at the Phase 3 and Phase 4 merge gates;
   do not silently report an unsupported lane as passing.
5. Keep native-GPU tests outside sanitizer presets unless the platform combination is
   known to work.

#### Done when

Both presets configure, build, and run, their current findings are written down, and
the concurrency/lifetime suites needed by Phases 3 and 4 are clean before those
phases begin.

### 5.4 State the toolchain floor

#### Why

`cmake/PciProjectOptions.cmake` requests `cxx_std_23` and no minimum compiler
version is stated anywhere in the build. Section 10.2 makes `std::expected` a
load-bearing part of the import API, and it is the least portable C++23 library
feature in common use: GCC 12+, Clang 16+ with libc++ 16+, or MSVC 19.33+. Finding
that out during Phase 5 on a contributor's machine is avoidable.

#### How

1. Add a `CMAKE_CXX_COMPILER_VERSION` floor check with a clear `FATAL_ERROR`.
2. Add a compile-time `__cpp_lib_expected` feature check at configure time.
3. Record the supported compiler matrix in the README and in CI.

#### Done when

An unsupported toolchain fails at configure time with a message naming the required
version.

### 5.5 Add a qualification comparison tool

#### Why

Section 12 currently asks for "no material regression" against a stored baseline.
"Material" has no number, the comparison is a human reading a Markdown table against
three JSON files, and `qualification/` contains no tool that compares anything. That
is not a gate. It is also the only safety mechanism Phase 4 has, and Phase 4 is the
phase most likely to regress performance.

#### How

1. Add `pci_qualification_diff` to `tools/`. It takes a baseline report JSON, a
   candidate report JSON, and a tolerance table; prints a per-measurement delta; and
   exits non-zero on regression.
2. Validate comparability before calculating deltas. Fail with a distinct diagnostic
   when schema, status, selected backend, validation mode, CPU/GPU budgets, source
   count, source bytes/points, or full-detail admission mode differ. Paths and
   timestamps may differ; workload identity comes from stable source metadata, not
   an absolute path. Implement schema-specific field maps for the native and
   headless reports rather than silently treating a missing metric as zero.
3. Commit the tolerance table. Proposed starting values, to be tuned against a
   same-host repeat run before they are trusted:

   | Measurement | Tolerance |
   |---|---|
   | First point from any/all sources | +10% |
   | All sources display-ready | +10% |
   | Sampled frame p95 | +15% |
   | Sampled frame max | +25% |
   | Peak CPU page residency | +5% |
   | Peak GPU point residency | +5% |
   | Settled visible draw calls | +5% |
   | Full-detail hierarchy pages | exact match |
   | GPU evictions | exact match |

4. State the caveat prominently: the stored baseline was measured on an Apple M4 Pro
   with Metal, a 4 GiB CPU page budget, and a 512 MiB GPU budget. Cross-host
   comparison is meaningless. The tool compares a fresh local baseline against a
   fresh local candidate on the same machine.
5. Register self-checks that:
   - diff each stored baseline against itself and expect success;
   - perturb one measurement beyond tolerance and expect a regression exit;
   - change one comparability field and expect a workload-mismatch exit;
   - remove a required field and expect a schema-error exit.

#### Done when

A performance regression produces a non-zero exit code and a named measurement rather
than a judgment call.

### 5.6 Put documentation back under version control, then archive it

#### Why

Two separate problems, and the order matters because the second one is a trap.

First, `.gitignore:74` contains `/docs/`. Ignore rules do not untrack files already in
Git, so several older files under `docs/superpowers/` remain tracked. They do,
however, hide every newer untracked document from normal `git status`, including
this guide, the codebase review it derives from, and several later plans and
findings. Git history shows files under `docs/` were tracked across 15 commits before
the ignore rule was added, so the current mixed state is a regression rather than a
coherent documentation policy. A 65-commit playbook that is ignored cannot be
reviewed, referenced reliably from commit messages, or amended with history.

Second, the repository root holds `RENDER_PIPELINE_IMPROVEMENT_PLAN.md` (114 KB),
`VECTOR_LAYER_OVERLAY_PLAN.md` (128 KB), `HIERARCHICAL_POINT_SOURCE_DESIGN.md`, and
`LOCAL_POINT_PAGE_STORE_DESIGN.md`. These are the first documents a new contributor
or agent reads, and they describe a state the code has moved past — for example,
`RENDER_PIPELINE_IMPROVEMENT_PLAN.md` still presents `DecodedBlockCache` as the
active document cache and names a `tests/unit/DecodedBlockCacheTests.cpp` that does
not exist. Deferring this to the end of the program, as the original draft did, means
every step in between is planned against contradictory documentation.

The trap: all four root documents are tracked, while a new destination under
`docs/archive/` is ignored. Moving them there **before** fixing the ignore rule can
produce a staged deletion with an ignored destination rather than the intended
rename. Fix the ignore rule first.

#### How

Commit one — restore tracking:

1. Remove `/docs/` from `.gitignore`.
2. Inspect the newly visible untracked files with `git status --short` and classify
   each as active documentation, historical documentation, or local scratch output.
   Do not blindly add the whole directory merely because it is now visible.
3. Add this guide and the codebase review it derives from. Add or archive the other
   reviewed documents intentionally; remove local scratch output or give it a narrow
   ignore rule.
4. If the rule was added to keep build output out of the tree, replace it with a
   narrow pattern for that output rather than the whole directory.

Commit two — archive the superseded root documents:

5. Move all four into `docs/archive/` with `git mv`, so history follows them.
6. Add a one-line status header to each: what it was, what superseded it, and its
   date.
7. Leave `README.md`, `UI_DESIGN_LANGUAGE.md`, and `qualification/README.md` in
   place; they describe current behavior.

#### Done when

`git status` reports documentation changes, every document at the repository root
describes the current system, and the superseded plans are reachable in
`docs/archive/` with their history intact.

### Phase 0 checkpoint

CI is green on the `ci` preset and the local `development` preset remains green.
Sanitizer baselines are recorded.
`pci_qualification_diff` reproduces the stored baseline within tolerance on the
development host. Root documentation no longer contradicts the code.

## 6. Phase 1: remove migration debt and centralize primitives

This phase is deliberately low risk. It reduces the number of APIs later phases
must move and establishes small reusable utilities without changing architecture.

### 6.1 Remove the legacy task-list path

#### Why

`PointCloudLayerPanel` contains both the older point-only
`PointCloudLoadJobState` presentation and the newer unified `LoadJobRow`
presentation. Production uses `setLoadRows()`, but the legacy fields, callbacks,
row builder, and context-menu branch remain. `MainWindow` also installs both sets
of callbacks (`src/app/MainWindow.cpp:349-356`).

This duplication makes every Tasks UI change require reasoning about two models and
allows a bare numeric job ID to be confused between point and vector jobs.

#### How

1. Add or retain tests in `tests/ui/MainWindowTests.cpp` that cover point and vector
   Cancel, Retry, Prioritize, and Dismiss actions through `LoadJobKey`.
2. Confirm `MainWindow::refreshLayerPanel()` supplies only `LoadJobRow` objects.
3. Remove `PointCloudLayerPanel::setLoadStates()`.
4. Remove `loadStates_` and the point-only phase-label helper used solely by that
   method.
5. Remove these callback setters and members:
   - `setCancelLoadCallback`;
   - `setRetryLoadCallback`;
   - `setPrioritizeLoadCallback`;
   - `setDismissLoadCallback`;
   - their `std::function<void(std::uint64_t)>` members.
6. Simplify `showLoadContextMenu()` to resolve one `LoadJobKey` and one
   `LoadJobRow`; delete the `loadRows_.empty()` compatibility branch.
7. Remove the corresponding legacy callback installation from `MainWindow`.
8. Search for the deleted names with `rg` and require zero results outside archived
   documents.

#### Tests

Run `pcinspector_ui_tests` and verify both inline task buttons and the context menu
route actions through the discriminated key.

#### Done when

There is one task-row representation, one set of task action signals/callbacks, and
no UI operation accepts a bare job ID without its kind.

### 6.2 Consolidate application construction around `ImportServices`

#### Why

`MainWindow` has three construction paths (`src/app/MainWindow.h:37-64`). The default
path creates concrete PDAL/OGR services internally, while the compatibility overload
accepts only a point controller and fabricates the vector half of the bundle. Most UI
tests still use the latter path. Both hide the invariant that point and vector
controllers share exactly one scheduler, and both put composition-root decisions in
a widget.

#### How

1. Add a test helper in UI test support that creates an `ImportServices` bundle
   from fake point and vector loaders plus one scheduler.
2. Move production composition to `main.cpp` (later
   `ApplicationBootstrap`): construct the concrete `PdalPointCloudLoader`,
   `OgrVectorLoader`, one scheduler, both controllers, and then `MainWindow`.
3. Migrate every `MainWindow` test to explicit `ImportServices` injection.
4. Remove the helper that converts a point-only controller into services.
5. Remove both service-fabricating `MainWindow` constructors: the default-loader
   overload and the point-controller-only compatibility overload. Retain only the
   constructor taking a complete `ImportServices`.
6. Make ownership concrete rather than documenting a shared-ownership convention:
   `ImportServices` uniquely owns the scheduler, controllers hold a non-owning
   reference/pointer to it, and member order plus `shutdown()` destroy controllers
   before waiting for and destroying the scheduler. If scheduler sharing outside
   this aggregate proves necessary, document that caller and keep `shared_ptr`; do
   not use shared ownership by default merely for convenient construction.
   Controllers owned by `unique_ptr` must have no QObject parent; never combine Qt
   parent ownership with a C++ owning smart pointer for the same object.
7. Remove `PointCloudLoadController` constructors that choose `PdalPointCloudLoader`
   or silently allocate a private scheduler once no repository caller uses them.
   Keep only constructors that receive a loader and scheduler explicitly.
8. Preserve `ImportServices::valid()` and the existing mismatch tests, updating
   identity checks to compare scheduler addresses if controllers borrow it.

#### Tests

Run async controller tests and all `MainWindow` tests. Add a shutdown test proving
both controllers are destroyed before the owned scheduler waits for idle.

#### Done when

Every production and test construction has one visible scheduler owner, and no
controller can accidentally create a competing worker pool.

### 6.3 Clarify document emptiness

#### Why

`SceneDocument::empty()` currently means "contains no point-cloud layers," while
`hasAnyLayer()` includes vectors. The special meaning is used to preserve vector
overlays during point-cloud replacement, but the generic name is misleading.

#### How

1. Add tests for a vector-only document:
   - it has scene content;
   - it has no point-cloud layers;
   - opening a point cloud chooses the intended replace/add behavior;
   - preserved vector layers retain IDs and styles during replacement.
2. Add `hasPointCloudLayers()` and return `!layers_.empty()`.
3. Replace each `document_->empty()` call according to intent:
   - use `!hasPointCloudLayers()` for point-load mode decisions;
   - use `!hasAnyLayer()` for empty-scene presentation;
   - use explicit layer counts where ordering logic depends on a count.
4. Remove `empty()` after all callers are migrated.
5. Update comments to state why point replacement preserves vector layers.

#### Tests

Run scene-document tests and MainWindow vector/replace tests.

#### Done when

No method named `empty()` has domain-specific semantics, and every caller expresses
whether it cares about point layers or all scene layers.

### 6.4 Retire the unused single-source decoded cache

#### Why

`DecodedBlockCache` and `DecodedPageCache` implement nearly the same LRU accounting.
Production document residency uses the multi-source `DecodedPageCache`; the older
class remains primarily as a directly tested implementation and as the owner of a
metrics type and default constant.

#### Known consumers

This step is a two-commit sequence because the type is more entangled than its
"unused" description suggests. Every one of these must be migrated before the class
can be deleted:

- `src/scene/DecodedPageCache.h:3` includes `DecodedBlockCache.h`;
- `src/scene/DecodedPageCache.h:34` defaults its budget to
  `DecodedBlockCache::defaultByteBudget`;
- `src/scene/DecodedPageCache.h` and `.cpp` return `DecodedBlockCacheMetrics`;
- `src/scene/PointCloudScene.h:64,81` use both the metrics type and the constant;
- `src/scene/SceneDocument.h:43` uses the metrics type;
- `tests/gpu/PointPickerGpuTests.cpp:273` uses the constant.

That last one matters: deleting the class without touching it breaks the
`native-gpu` build, which section 12's matrix would not otherwise tell you to run.

#### How

Commit one — relocate the shared vocabulary:

1. Move `DecodedBlockCacheMetrics` to `DecodedPageCache.h` or a small
   `DecodedCacheMetrics.h` value header.
2. Move the default decoded-cache byte budget to the active cache or a residency
   configuration header.
3. Update all six consumers above, including the GPU test.

Commit two — remove the implementation:

4. Port the `DecodedBlockCache` unit scenarios, which live inside
   `tests/unit/PointCloudHierarchyTests.cpp` (there is no dedicated test file), to
   `DecodedPageCache` using a fixed non-zero `PointCloudSourceId`.
5. Preserve tests for insertion, replacement, LRU order, leased payloads,
   protected/pinned entries, byte limits, and metric counters.
6. Delete `DecodedBlockCache.cpp/.h` and remove them from `src/CMakeLists.txt`.

Do not introduce a generic cache template in this step. First remove the dead
implementation while keeping the proven active behavior.

#### Tests

Run hierarchy, scene-document, residency benchmark, and async loading tests, then
build and run the `native-gpu` preset because of the GPU test dependency above.

#### Done when

There is one decoded payload cache implementation and all production cache metrics
come from it.

### 6.5 Remove unused hierarchy compatibility methods

#### Why

`PointCloudScene` exposes multiple ways to install hierarchy resources, including
wrappers no longer used by production. Extra lifecycle entry points make it harder
to prove that decode callbacks cannot outlive their scheduler or cache.

#### How

1. Use `rg` to classify callers of:
   - `setHierarchyResidencyCoordinator`;
   - `setDocumentHierarchyResources`;
   - `useStandaloneHierarchyResidency`;
   - `setHierarchyResidencyActive`;
   - `syncHierarchyResidencyBudget`.
2. Keep the document-resource installation, standalone fallback, visibility
   activation, and budget synchronization paths that are part of real lifecycle.
3. Remove wrappers with no callers after migrating any isolated tests to the real
   lifecycle method.
4. Document the remaining state transitions next to the API:
   standalone -> document-owned -> standalone -> destruction.

#### Tests

Run hierarchy cancellation, document add/remove, root-budget, and destruction
tests under the `tsan` preset added in section 5.3.

#### Done when

Each remaining method represents a distinct lifecycle transition with a production
caller and a test.

### 6.6 Add checked and saturating arithmetic primitives

#### Why

Saturating addition and multiplication are independently implemented across memory
policy, scene metrics, render selection, import admission, local indexing, source
metrics, and vector geometry. The desired overflow policy differs by context:
metrics saturate, while allocations and untrusted input validation often fail.

#### How

1. Add a small header in the future foundation target with unsigned-integral
   primitives:

   ```cpp
   template<std::unsigned_integral T>
   constexpr T saturatingAdd(T left, T right) noexcept;

   template<std::unsigned_integral T>
   constexpr T saturatingMultiply(T left, T right) noexcept;

   template<std::unsigned_integral T>
   constexpr std::optional<T> checkedAdd(T left, T right) noexcept;
   ```

2. Add boundary tests for zero, maximum, exact fit, and overflow.
3. Replace metric/accounting helpers with saturating operations.
4. Replace validation helpers with checked operations followed by the existing
   domain-specific exception.
5. Do not convert throwing behavior to saturation or vice versa during this pass.
6. Remove local helpers only after their callers have migrated.

#### Tests

Run core overflow tests, vector limit tests, document extreme-total tests, scheduler
metrics tests, and local-page component tests.

#### Done when

Overflow mechanics are centralized while each domain still explicitly chooses its
overflow policy.

### 6.7 Centralize hash combination

#### Why

Stable composite keys recur in decoded pages, GPU blocks, and future strong IDs.
Hand-written hash combination is easy to vary accidentally.

#### How

1. Add `hashCombine(seed, value)` to the foundation target.
2. Keep key-specific hash functors but implement them through the shared primitive.
3. Test equality implies equal hashes and exercise keys differing in every field.
4. Do not persist `std::hash` output or treat it as a disk-format identifier.

#### Done when

All in-memory composite hashes share one implementation and persistent IDs continue
to use their explicitly defined algorithms.

### 6.8 Centralize Qt and filesystem path conversion

#### Why

The application repeatedly uses `QString::fromStdString(path.string())`. That is not
a reliable Windows native-path conversion and duplicates display-name rules.

#### How

1. Add a QtCore-boundary helper with:
   - `pathToQString(const std::filesystem::path&)`;
   - `qStringToPath(QStringView)`;
   - `displayPathName(const std::filesystem::path&)`.
2. Use wide strings on Windows and native UTF-8 bytes on the supported Unix path.
3. Replace conversions in startup, controllers, dialogs, task rows, diagnostics,
   and qualification output.
4. Keep path conversion out of pure domain targets.
5. Add tests for empty paths, non-ASCII filenames, filename fallback, and round trip.

#### Done when

Filesystem/UI conversions are platform-correct and defined in one Qt boundary.
Treat any newly passing non-ASCII Windows case as an explicit correctness fix, not as
proof that this was a behavior-neutral mechanical replacement.

### 6.9 Add an advisory static-analysis preset

#### Why

Introducing `clang-tidy` late means its first run lands on freshly moved code and its
findings cannot be distinguished from refactor damage. Introducing it now produces a
baseline against the code as it stands.

#### How

Add `.clang-tidy` with an initially focused set:

- `bugprone-*`;
- `performance-*`;
- `modernize-use-nullptr`;
- `modernize-use-override`;
- `modernize-use-equals-default`;
- selected readability checks that do not impose a new naming convention.

Run it as an advisory preset and record the baseline. Do not gate. Avoid enabling
broad conversion/narrowing checks over third-party headers. Promotion to gating is
section 11.1, at the end of the program.

### Phase 1 checkpoint

Run the complete development and CI builds plus the `native-gpu` build, which
section 6.4 touches. Production behavior must be unchanged, while obsolete task,
construction, cache, and lifecycle APIs are gone. The only intentional behavior
change in this phase is any separately committed platform-path correctness fix from
section 6.8.

## 7. Phase 2: make target boundaries match responsibilities

The existing CMake split is a good start, but several dependencies are artifacts of
file placement rather than domain ownership. This phase changes target ownership
without redesigning algorithms.

### 7.1 Define the target graph

Arrows point from a target to the targets it depends on. `application` and
`renderer_rhi` are **siblings**, both consumed by the executable; the application
must never depend on `renderer_rhi`. That separation exists in the code today and is
the property section 7.6 defends.

Targets are assigned to layers. **Every dependency edge points strictly downward.**
A target may not depend on a peer in its own layer, or on anything above it.

```text
layer 7   executable
layer 6   application            renderer_rhi
layer 5   import_async
layer 4   app_config   app_model   import_pdal   import_ogr
layer 3   import_api  renderer_api  renderer_planning  development_support
layer 2   scene
layer 1   pointcloud  vector  tasking  navigation  platform
layer 0   foundation
```

A layer number is an upper bound, not a requirement: `import_ogr` sits at layer 4 for
readability but depends only on `vector`. The rule that matters is the direction.

The adjacency list is the authoritative form, because section 7.7 turns it into a
build rule:

```text
executable        -> application, renderer_rhi, import_async, import_pdal,
                     import_ogr, app_config, development_support, platform
application       -> app_config, app_model, import_async, renderer_api,
                     scene, pointcloud, vector, platform,
                     Qt6::Concurrent (private), Qt6::Widgets
app_config        -> renderer_api, platform, foundation, Qt6::Core
renderer_rhi      -> renderer_api, renderer_planning, scene, navigation,
                     pointcloud, vector, platform,
                     Qt6::GuiPrivate (private), Qt6::Widgets
renderer_planning -> scene, pointcloud, foundation
renderer_api      -> scene, Qt6::Core
app_model         -> import_api, renderer_api
import_async      -> import_api, tasking, platform, Qt6::Core
import_pdal       -> import_api, Qt6::Core (private), PDAL (private)
import_ogr        -> vector, GDAL (private)
import_api        -> scene
scene             -> pointcloud, vector, tasking, foundation
tasking           -> foundation
navigation        -> foundation
pointcloud        -> foundation
vector            -> foundation, earcut (private)
platform          -> foundation, standard library, OS
foundation        -> (standard library)
development_support -> scene, pointcloud
```

The exact names may keep the existing `pcinspector_` prefix, but aliases should
remain consistently available as `pcinspector::<component>`.

Two notes on the graph as drawn:

- `renderer_api` currently owns only `RenderViewport.h`, but `app/` also includes
  `renderer/RenderLoadProgress.h` and `renderer/PointColorLabels.h`, which are
  compiled into the renderer target and reachable only because every target puts
  `src/` on its include path. Move those two value headers into `renderer_api` as
  part of this phase; they are the concrete instance of the problem section 7.7
  describes.
- `platform` (`SystemMemoryInfo`, `ProcessMemory`) is consumed by application
  bootstrap and memory-budget policy, by import-controller metrics, and by renderer
  telemetry. Keep it a leaf and make those direct dependencies explicit.
- Concrete PDAL and OGR adapters are dependencies of the executable composition
  root, not of `import_async` or `application`. This requires section 6.2 to remove
  the controller and window constructors that instantiate adapters internally.
  Tests can then compose fake adapters without either external library.

### 7.2 Create `foundation`

#### Why

`Vec3d`, `Bounds3d`, overflow helpers, in-memory hash helpers, and strong-ID support
are shared vocabulary, not application options or point-cloud policy. Keeping
`Bounds3d` in `pointcloud` currently forces `vector` to depend on the point-cloud
target for nothing but a bounding box.

#### How

1. Create `pcinspector_foundation` containing:
   - `Vec3d`;
   - `Bounds3d`;
   - checked/saturating arithmetic from section 6.6;
   - hash combination from section 6.7;
   - `StrongId`.
2. Add `StrongId<Tag, Representation = std::uint64_t>` here, in its own commit:
   - explicit construction and `.value()` access;
   - **no** implicit integer conversion;
   - defaulted comparison;
   - `std::hash` specialization built on `hashCombine`.
   Keep the wrapper policy-free: it does not decide whether zero is valid. At each
   adopting subsystem, replace "zero means none" fields and parameters with
   `std::optional<StrongId<...>>`; where a disk or JSON schema reserves zero, enforce
   that rule at the serialization boundary.

   This type is introduced in Phase 2 rather than Phase 5 because section 8.1's
   unified layer model uses it. Adopting it across subsystems remains section 10.3;
   only the type lands here.
3. Extend `Bounds3d` with small operations currently duplicated elsewhere:
   `extend`, center, maximum extent, and validation. Keep render-specific culling
   outside this type.
4. Update point-cloud, vector, scene, import, and renderer targets to link foundation.
5. Remove the point-cloud dependency from vector.
6. Keep the source move and include rewrite in one mechanical commit after tests
   compile against the new target.

#### Tests

Move vector, bounds, and math tests to link only foundation/vector as appropriate.
Add compile-time assertions that distinct `StrongId` tags are not interconvertible.
Verify that no Qt, PDAL, GDAL, or QRhi usage requirement reaches foundation.

#### Done when

Shared geometry has a neutral owner, `vector` no longer depends on point-cloud policy
merely to represent bounds, and `StrongId` exists with tests but no adopters.

### 7.3 Split platform and navigation code out of `core`

#### Why

The current `core` target mixes CLI parsing, OS memory queries, navigation, adaptive
point budgets, synthetic data, and GPU point layout. A target named "core" tends to
become a dependency dumping ground.

#### How

1. Create `platform` for `SystemMemoryInfo` and `ProcessMemory`.
2. Create `navigation` for `NavigationCamera` and `NavigationInputState`.
3. Move `AdaptivePointBudget` to `renderer_planning`, **not** to `navigation`. Its
   only production consumer is `RenderViewportWidget`
   (`src/renderer/rhi/RenderViewportWidget.h` and `.cpp`; `tests/unit/CoreTests.cpp`
   is the only other user). It is render policy that happens to react to camera
   motion, and filing it under navigation would recreate the same misplacement this
   phase exists to fix.
4. Move `GpuPoint` and `GpuPointProperties` to point-cloud domain code because their
   binary contract describes point payloads. Keep the 16-byte assertion with them.
5. Move synthetic point generation either beside point-cloud fixtures or into a
   small development-support target; do not make production scene code depend on
   benchmark-only helpers.
6. Split the contents of `AppOptions` by responsibility instead of moving the file
   intact:
   - move the `GraphicsApi` value type and backend-name vocabulary to
     `renderer_api`;
   - move numeric parsing, memory-option parsing, and path expansion into
     `app_config` (tools that reuse them link that target explicitly);
   - let section 8.7 replace those free parser functions with
     `ApplicationConfig` parsing, without moving renderer vocabulary back upward.
7. Move `MemoryBudgetPolicy` into `app_config`; it depends on `platform` for
   `SystemMemoryInfo` and on foundation arithmetic, and is application policy rather
   than an OS query or scene invariant.
8. Ensure direct consumers of `ProcessMemory` link `platform`: today these include
   `import_async`, `renderer_rhi`, and the benchmark tools.
9. Delete `pcinspector_core` once no source remains whose ownership is genuinely
   shared.

#### Done when

Every former core file has an owner named after its responsibility, and target links
describe actual usage.

### 7.4 Move the scheduler into `tasking`

#### Why

`PointTaskScheduler` schedules point imports, vector imports, and hierarchy decodes.
Its current name and location in `scene` create a false dependency from import to
scene.

#### How

1. Add characterization tests for priority, byte admission, fairness groups,
   reprioritization, queued cancellation, shutdown, and exception containment. Run
   them under the `tsan` preset.
2. Rename the type to `TaskScheduler` and `PointTaskPriority` to `TaskPriority`.
3. Move it to the `tasking` target, which depends only on foundation and the standard
   library.
4. Update `ImportServices`, both controllers, `SceneDocument`, and
   `PointCloudScene` to depend on tasking.
5. Preserve stop-token behavior and the rule that active tasks cancel cooperatively.
6. Add a failure metric or error callback only as a separate behavioral change; do
   not silently redefine "completed" metrics during the move.

#### Done when

Import no longer depends on scene just to run background work, and scheduler tests
link no Qt or point-cloud code.

### 7.5 Create a QRhi-free renderer-planning target

#### Why

Frustum culling currently lives in scene, while `FrameCamera` lives below `rhi`
despite containing no QRhi code. Frame allocation, selection, measurement, and
culling are portable renderer policy and should compile without Qt private headers.

This move is already almost free: `FramePlanner.h`, `RenderSelection.h`,
`Measurement.h`, `FrustumCuller.h`, and `FrameCamera.h` include nothing but
`core/Vec3d.h`, `pointcloud/Bounds3d.h`, `scene/PointCloudNode.h`, and standard
headers. None of them includes Qt.

#### How

1. Create `renderer_planning` with:
   - `FrustumCuller`;
   - `FrameCamera`;
   - `FramePlanner`;
   - `RenderSelection`;
   - `Measurement`;
   - `AdaptivePointBudget` (from section 7.3);
   - render-activity policy.
2. Move their tests to a target that links renderer-planning directly, without
   `Qt6::GuiPrivate`.
3. Keep QRhi buffers, shaders, pipelines, resource batches, and widgets in
   `renderer_rhi`.
4. Keep `RenderViewport` in `renderer_api`, and move `RenderLoadProgress.h` and
   `PointColorLabels.h` there too (section 7.1).
5. Check that renderer-planning public headers include only foundation,
   point-cloud/scene value contracts, and the standard library.

#### Done when

The complete LOD/fairness/culling/measurement policy suite builds without Qt private
headers or a GPU.

### 7.6 Hide QRhi implementation headers

#### Why

The public concrete widget header derives from `QRhiWidget`, so consumers and tests
inherit Qt private GUI requirements, and `pcinspector_renderer` propagates
`Qt6::GuiPrivate` as a PUBLIC usage requirement. This weakens the otherwise useful
`RenderViewport` abstraction.

#### How

1. Keep `createRenderViewport()` as the production construction boundary.
2. Make the concrete QRhi widget an implementation-private type.
3. Move tests that genuinely need the concrete type into a renderer-internal test
   target with explicit access to private headers.
4. Replace useful `*ForTesting()` getters with one of:
   - a production-meaningful immutable viewport state snapshot;
   - observation through the existing API;
   - an internal fixture compiled only into renderer tests.
5. Link `Qt6::GuiPrivate` privately from `renderer_rhi` wherever static-library
   propagation permits. It must not be a usage requirement of `renderer_api`.
6. Ensure application UI includes no QRhi header. This holds today; add the check
   that keeps it holding.

#### Done when

Normal application and API tests compile against `RenderViewport` alone, and QRhi
private version coupling is contained in one target.

### 7.7 Enforce dependency boundaries, in `src` and in tests

#### Why

Architecture that exists only in a diagram will erode. A small automated include
check catches accidental PDAL, GDAL, Qt, or QRhi leakage earlier than linker errors.

The boundary erodes through tests as readily as through production code. Today
`pcinspector_unit_tests` links `pcinspector::app_model`, which transitively pulls
in `import_api` -> `scene` -> `core`, `pointcloud`, `vector`, and `renderer_api`. So
`FrustumCullerTests`, `VectorGeometryTests`, and `CoreTests` all compile against
nearly the whole tree, and a new dependency added anywhere is invisible to them. A
check that scans only `src` would declare victory while the test suite quietly holds
the old graph in place.

#### How

1. Add a CMake/CTest script that scans project includes and fails on:
   - PDAL headers outside `import/pdal` and fixture code;
   - GDAL/OGR headers outside `import/ogr` and fixture code;
   - QRhi headers outside `renderer/rhi` and internal GPU tests;
   - Qt headers in foundation, point-cloud, vector, tasking, and portable planning.
2. Add a CMake target-edge check as a separate mechanism. Inspect each project's
   direct `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` entries, normalize aliases,
   and compare them with the allow-list in section 7.1. Include scanning catches
   header leakage; it cannot prove the link graph by itself.
3. Adopt and enforce this rule: **each test target links exactly the target under
   test plus its declared dependencies.** Split `pcinspector_unit_tests` into
   per-target test executables accordingly — this is a deliverable of this phase, not
   incidental cleanup.
4. Express public headers with CMake header file sets or explicit interface lists.
5. Stop treating the entire `src` tree as every target's conceptual public API. The
   `app/` -> `renderer/RenderLoadProgress.h` include noted in section 7.1 works only
   because of this; fixing the include path is what makes the rule enforceable.
6. Run both boundary tests in the ordinary non-GPU suite and in CI.

#### Done when

Adding a link or an include that contradicts section 7.1's adjacency list fails a
test, from production code and from test code alike.

### Phase 2 checkpoint

Run the full suite from a clean build directory to catch missing transitive includes.
No runtime or UI behavior should have changed. Before opening the Phase 3 branch,
the scheduler, controller, cache, and scene sanitizer suites identified in section
5.3 must be clean and running in their supported required lane.

## 8. Phase 3: unify scene state and decompose application orchestration

This phase establishes the document model that both the UI and the renderer read,
then removes workflow state from widgets. It should begin only after Phase 1 has
removed compatibility paths, otherwise those paths will be copied into the new
classes.

The scene-layer model and snapshot come **first**, before `SceneSession`. In the
original draft they were the opening of Phase 4, while `SceneSession` — specified in
Phase 3 in terms of `SceneDocumentSnapshot` — was written against a type that did not
yet exist. Building the model first means `SceneSession`, the docks, and the Qt models
are each written once against their final input.

### 8.1 Store one ordered scene-layer collection

#### Why

`SceneDocument` maintains separate point and vector vectors while sharing one ID
space. Visibility, removal, isolation, bounds, UI listing, lookup, and copy behavior
therefore contain parallel loops and methods. Separate collections also force UI
ordering policy outside the document.

#### Proposed model

```cpp
using SceneLayerId = StrongId<struct SceneLayerTag>;

struct PointCloudLayerState {
    PointCloudScenePtr scene;
    PointColorMode colorMode;
    PointClassificationFilter classificationFilter;
};

struct VectorLayerState {
    VectorLayerDataPtr data;
    VectorLayerStyle style;
};

struct SceneLayer {
    SceneLayerId id;
    bool visible = true;
    std::variant<PointCloudLayerState, VectorLayerState> payload;
};
```

`StrongId` comes from foundation (section 7.2). `SceneLayerId` currently exists as a
`std::uint64_t` alias at `src/scene/SceneDocument.h:22`; this step changes what the
alias names, not whether it exists. Callers that already spell `SceneLayerId` need
minimal type-name churn, but raw-zero sentinels, `quint64` signal signatures, JSON
serialization, and test literals still require explicit migration because the strong
type deliberately has no implicit integer conversion.

#### How

1. Add tests fixing desired cross-kind insertion and display order.
2. Introduce `SceneLayer` and conversion helpers while retaining old accessors.
3. Store new layers in one `std::vector<SceneLayer>`.
4. Reimplement lookup, visibility, remove, isolate, show-all, bounds, and kind through
   the variant.
5. Preserve point-specific and vector-specific accessors temporarily as projections
   for existing renderer/UI callers.
6. Migrate callers to the unified snapshot described next.
7. Replace absent/selected-layer zero sentinels with `std::optional<SceneLayerId>`,
   register the Qt metatype, and serialize `.value()` explicitly where the existing
   JSON schema requires a number.
8. Remove the old parallel vectors and revision bookkeeping only after renderer and
   UI tests use the new model.

Use `std::visit` for kind-specific policy and ordinary common code for ID,
visibility, ordering, and removal. Do not introduce a virtual base class for this
closed set of layer alternatives.

#### Done when

Common layer operations have one implementation and document order is authoritative.

### 8.2 Add a structurally immutable `SceneDocumentSnapshot`

#### Why

The renderer currently asks for copied point and vector vectors every frame, then
maintains additional caches keyed by several revisions. UI presentation obtains
separate copies as well. A cached, structurally immutable document snapshot creates
one coherent read contract and avoids repeated unchanged work. "Structurally
immutable" is deliberate: a snapshot fixes layer membership, order, visibility, and
style, but its `PointCloudScenePtr` payloads continue to publish worker-produced
scene snapshots. Deep-copying live point scenes would be both incorrect and
prohibitively expensive.

#### Proposed contract

```cpp
struct SceneDocumentSnapshot {
    std::uint64_t revision = 0;
    std::uint64_t pointRevision = 0;
    std::uint64_t vectorRevision = 0;
    std::vector<SceneLayer> layers;
    std::optional<Bounds3d> bounds;
    std::optional<Bounds3d> visibleBounds;
    std::uint64_t visibleExpectedPointCount = 0;
    std::uint64_t decodedByteBudget = 0;
};

using SceneDocumentSnapshotPtr =
    std::shared_ptr<const SceneDocumentSnapshot>;

SceneDocumentSnapshotPtr SceneDocument::snapshot() const;
```

#### How

1. Define revision semantics before migrating callers: `revision` advances for
   **every** document-structural or style mutation, `pointRevision` advances for
   point-affecting mutations, and `vectorRevision` advances for vector-affecting
   mutations. Test vector-only changes against the overall revision; the current
   implementation has separate point/vector counters and no combined revision.
2. Invalidate the cached snapshot whenever layer membership, order, visibility,
   point style/filter, or vector style changes.
3. Rebuild it once on the next `snapshot()` call and return the same shared pointer
   for subsequent unchanged reads. The application owner normally requests the new
   snapshot immediately after a successful command so observers still receive an
   eager notification without rebuilding inside every mutation method.
4. Keep point-scene payload snapshots separate because worker-published scene
   revisions change independently of document structure.
5. Replace `RenderViewport::setDocument(SceneDocumentPtr, ...)` with a snapshot-based
   input and migrate every current document read explicitly:
   - membership, order, styles, bounds, point counts, decoded budget, stale-pick
     revisions, and empty-scene checks come from `SceneDocumentSnapshot`;
   - per-scene revisions and decoded payloads come from each point scene's existing
     immutable scene snapshot;
   - frustum candidate indexing moves into renderer planning as a cache keyed by the
     document revision, rather than calling `visibleLayersIntersecting()` on the
     mutable document;
   - aggregate document/cache/scheduler metrics are sampled by the application owner
     through the existing `SceneDocument::hierarchyMetrics()` boundary (initially
     `MainWindow`, then the application workflow owner after its extraction) and
     merged with GPU/frame metrics by the existing reporting code. A later
     application-shell step extracts the formatter and reporter after this behavior
     is working. Preserve the existing qualification JSON projection while changing
     its internal producers.
6. Migrate UI models and renderer membership refresh to the same structural
   snapshot. Do not leave a mutable `SceneDocumentPtr` in the viewport as a shortcut;
   that would preserve the dual read paths this step exists to remove.
7. Remove duplicated point/vector revision polling where the snapshot now supplies a
   coherent value.
8. Verify hidden layers still contribute to automatic coordinate color bounds
   (contract 12).

#### Done when

Unchanged frames reuse one document snapshot, the viewport has no mutable document
handle, and UI/renderer observe identical layer membership, ordering, visibility,
and bounds.

### 8.3 Introduce `SceneSession`

#### Why

`MainWindow` currently owns widgets and also implements point batch scheduling,
transactional document replacement, progressive admission, rollback, render/import
progress reconciliation, memory-budget refresh, and job completion. These are
application workflows, not window presentation.

#### Proposed responsibilities

`SceneSession : QObject` owns:

- the current `SceneDocument`;
- `ImportServices`;
- the active point-load map and batch order;
- replace/add transaction state;
- automatic memory-budget refresh;
- reconciliation of import completion with first-frame/display-ready events;
- layer visibility, color, filter, style, isolate, show-all, and remove commands;
- unified `LoadJobRow` publication.

It emits value-oriented signals such as:

```cpp
void documentChanged(SceneDocumentSnapshotPtr snapshot,
                     bool frameVisibleLayers);
void taskRowsChanged(std::vector<LoadJobRow> rows);
void loadingProgressChanged(LoadingProgressState state);
void statusChanged(QString message);
void failureOccurred(QString title, QString message);
void vectorSelectionRequired(quint64 jobId, VectorImportPreflight preflight);
```

Every type in those signatures exists by this point: `SceneDocumentSnapshot` from
section 8.2, `LoadJobRow` and `VectorImportPreflight` from the current import target.
Declare/register the metatypes used by any queued Qt connection, including the
snapshot pointer, strong IDs, and row/preflight aggregates. Prefer direct connections
for session-to-widget traffic because both objects are GUI-thread-owned, but do not
make correctness depend on Qt silently accepting an unregistered queued payload.

#### How

1. Add characterization tests for:
   - one replace load;
   - add load;
   - replace batch with out-of-order previews;
   - cancellation before/after preview;
   - rollback on failure;
   - display-ready retirement;
   - vector partial success.
2. Extract the `ActiveLoad` structure and point-load handling methods unchanged into
   `SceneSession`.
3. Pass renderer progress into `SceneSession::onRenderLoadProgress()` rather than
   letting the window interpret it.
4. Move document mutations and layer-panel refresh preparation into the session.
   Feed `documentChanged` directly to the snapshot-based renderer input from section
   8.2 and to the UI models; neither consumer should query the mutable document.
5. Keep dialogs in `MainWindow`; for example, a vector preflight signal causes the
   window to show `VectorSublayerDialog` and return the selection to the session.
6. Move memory refresh timers to the session or inject timer ticks for deterministic
   tests.
7. Once tests call the session directly, remove migrated fields and methods from
   `MainWindow`.

#### Thread ownership

- `SceneSession` and `SceneDocument` mutations occur on the GUI/application thread.
- loaders run through `TaskScheduler` workers.
- controller completion is posted to the owning Qt thread before session mutation.
- `PointCloudScene` remains responsible for locking worker-published scene data.

Add debug assertions for these rules at mutation boundaries, and run the session
tests under the `tsan` preset.

#### Done when

The point-load state machine can be tested without constructing `QMainWindow`, and
`MainWindow` no longer owns `ActiveLoad` or batch transaction fields.

### 8.4 Reduce `MainWindow` to a shell

#### Why

After session extraction, the window should express layout and interaction rather
than application state transitions.

#### How

1. Group construction into focused private builders or small collaborators:
   menus/actions, central viewport, toolbar, docks, and status presentation.
2. Connect actions to typed `SceneSession` and `RenderViewport` commands.
3. Keep file and modal/non-modal dialog creation in the window.
4. Move workspace save/restore into `WorkspaceSettings` with stable keys.
5. Move diagnostic-line formatting into `RenderDiagnosticsFormatter`.
6. Move qualification accumulation and JSON writing into `QualificationReporter`.
   Keep the emitted schema byte-compatible; `pci_qualification_diff` reads it.
7. Keep compile-time diagnostic UI guards around composition, not around core
   loading behavior.

#### Tests

Retain UI tests for action identity, shortcuts, dock visibility, dialogs, tool
synchronization, object names, and view/session wiring. Move workflow assertions to
`SceneSession` tests.

#### Done when

The window owns Qt presentation objects but not import, transaction, progress, or
qualification algorithms.

### 8.5 Split `PointCloudLayerPanel`

#### Why

The class name describes one dock, while the 2,132-line object constructs and returns
Layers, Inspector, Tasks, and optional Diagnostics docks. It stores point layers,
vector layers, load rows, all property editors, and every callback.

This is the second-largest concentration in the codebase and it is being split four
ways. Preserving Qt object names, as the original draft proposed, keeps UI tests
compiling but proves almost nothing about behavior. Characterize first.

#### Behavior at risk, and currently untested

Write these characterization tests in `tests/ui/` **before** the first extraction:

- property-editor state retention across selection changes, including when the
  selected layer is removed while an editor is focused;
- per-layer callback routing: the correct `SceneLayerId` reaches the correct handler
  when layers are reordered or removed;
- layers/inspector selection synchronization in both directions;
- Tasks-dock action routing through `LoadJobKey` for both job kinds (already covered
  by section 6.1; assert it survives the split);
- context-menu construction for point layers, vector layers, and mixed selection;
- Diagnostics dock presence and absence under `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI`,
  since the `development` preset builds with it OFF and `native-gpu` with it ON;
- dock object names, visibility defaults, and restored layout.

#### Target docks

Each consumes a value input and emits typed signals; none holds a sibling.

- `SceneLayersDock`: consumes `std::shared_ptr<const SceneDocumentSnapshot>`; emits
  `selectionChanged(SceneLayerId)`, `visibilityToggled(SceneLayerId, bool)`,
  `isolateRequested(SceneLayerId)`, `showAllRequested()`,
  `removeRequested(SceneLayerId)`.
- `LayerInspectorDock`: consumes the selected `SceneLayer`; emits
  `pointColorModeChanged(SceneLayerId, PointColorMode)`,
  `classificationFilterChanged(SceneLayerId, PointClassificationFilter)`,
  `vectorStyleChanged(SceneLayerId, VectorLayerStyle)`.
- `TaskDock`: consumes `std::vector<LoadJobRow>`; emits one
  `jobActionRequested(LoadJobKey, LoadJobAction)`.
- `DiagnosticsDock`: consumes formatted diagnostic lines; emits nothing. Behind the
  existing compile-time option.

#### How

1. Land the characterization tests above.
2. Extract `SceneLayersDock` first.
3. Extract `LayerInspectorDock`.
4. Extract `TaskDock`.
5. Extract `DiagnosticsDock`.
6. Let `MainWindow` compose docks directly; remove the panel methods that return
   sibling docks.
7. Remove the original class after all responsibilities have owners.

#### Done when

No widget owns unrelated sibling docks, each dock can be tested independently, and
every behavior listed above still has a passing test.

### 8.6 Introduce incremental Qt models

#### Why

The current layer and task views clear and rebuild `QListWidget` rows. That couples
presentation to copied domain vectors, recreates row widgets, and complicates stable
selection.

#### How

1. Add `SceneLayerListModel : QAbstractListModel` projecting
   `SceneDocumentSnapshot`, with roles for ID, kind, name, visibility, warning state,
   and summary text.
2. Add `TaskListModel : QAbstractListModel` with roles derived from `LoadJobRow`.
3. Update models by stable ID using insert/remove/dataChanged instead of reset where
   possible.
4. Keep formatting in model projection helpers, not in scene/domain types.
5. Use delegates or focused row widgets for task actions.
6. Preserve selection by `SceneLayerId`, never by row number. Register the strong-ID
   Qt metatype needed for `QVariant` roles.
7. Add model tests using `QAbstractItemModelTester` where available.

#### Done when

An unchanged snapshot produces no row reconstruction, selection survives unrelated
updates, and view widgets contain no domain lookup loops.

### 8.7 Introduce `ApplicationConfig`

#### Why

`main.cpp` defines Qt CLI options, parses numeric values, calculates budgets, selects
cache paths, configures diagnostics, creates services, implements a synchronous
smoke-load path, and starts the window. At 324 lines it is not oversized, and this
step is not a size remedy — the point is that none of that behavior can currently be
tested without executing the application, and CLI option handling is a release
contract (contract 4).

It comes last in this phase because `main()` composes the viewport, session, and
window, so simplifying it is easiest once `SceneSession` exists.

#### Proposed interface

```cpp
struct ApplicationConfig {
    std::vector<std::filesystem::path> sources;
    std::uint64_t syntheticPointCount = 10'000'000;
    std::uint64_t maximumLoadPoints = 10'000'000;
    MemoryBudgetOption cpuBudget;
    std::uint64_t gpuByteBudget = 512ULL * 1024 * 1024;
    GraphicsApi graphicsApi = GraphicsApi::Auto;
    bool smokeTest = false;
    bool gpuValidation = false;
    std::optional<QualificationOptions> qualification;
};

struct ConfigEarlyExit {
    QString message;
    int exitCode = 0;
    bool writeToStandardError = false;
};

using ApplicationInvocation =
    std::variant<ApplicationConfig, ConfigEarlyExit>;

ApplicationInvocation
parseApplicationInvocation(const QStringList &arguments);
```

The parser uses a variant rather than `expected` because `--help` and `--version` are
successful early exits, not errors. Section 5.4 still establishes `std::expected`
support for the fallible value-producing APIs in section 10.2.

#### How

1. Write table-driven tests for all existing CLI success and failure cases, including
   exit codes, output stream, `--help`, and `--version` behavior.
2. Move option declaration and validation into a parser class/function that calls
   `QCommandLineParser::parse()` rather than an API that terminates the process.
   Return help/version/error text as `ConfigEarlyExit`, so tests never invoke
   `std::exit`.
3. Keep system-memory querying and automatic-budget calculation in a separate
   `resolveMemoryBudget(config, systemSnapshot)` function so it is deterministic in
   tests.
4. Move platform cache-path selection into a tested helper using the centralized
   Qt/filesystem conversion from section 6.8.
5. Move synchronous smoke-scene construction into an `ApplicationBootstrap` helper.
   The `renderer_smoke*` CTest cases assert on its stdout; keep the output stable.
6. Reduce `main()` to:
   - construct `QApplication`;
   - set identity/theme;
   - parse config;
   - print and return any `ConfigEarlyExit` exactly once;
   - load startup color maps;
   - compose viewport, session, and window;
   - enter the event loop.

#### Tests

Test missing values, numeric overflow, unsupported backends, glob expansion,
qualification dependencies, smoke incompatibilities, automatic budgets, and cache
fallback. Run the `native-gpu` smoke tests, which exercise the CLI end to end.

#### Done when

`main()` contains no domain policy and configuration errors can be tested without
showing a window.

### Phase 3 checkpoint

Run all UI and async tests. Sections 8.1 and 8.2 change what the renderer reads every
frame, so run the `native-gpu` preset and `pci_qualification_diff` at the end of this
phase — the original draft's "native GPU run is optional here" no longer applies now
that the layer model moved into Phase 3.

## 9. Phase 4: decompose render orchestration

This is the highest-risk structural phase. Keep commits small and run native GPU
qualification after the complete phase.

It is also the phase with the thinnest existing test coverage, which is why
section 9.3 records golden frame plans before the large extractions rather than
porting tests after them.

### 9.1 Separate flat and hierarchical scene storage

#### Why

`PointCloudScene` combines flat block accumulation and statistics with hierarchical
source queries, decode scheduling, page-cache leases, pins, failures, and
cancellation. Several methods are meaningful only in one mode and throw when called
in the other.

#### How

1. Characterize construction, flat publication, hierarchical loading, cancellation,
   pinning, errors, statistics, snapshots, and resource transitions.
2. Introduce internal `FlatSceneStorage` and `HierarchicalSceneStorage` types.
3. Store them behind `std::variant`; the alternatives are closed and selected at
   construction.
4. Keep `PointCloudScene` as the public facade for metadata, invalidation, revision,
   source-wide statistics, and renderer-facing snapshots.
5. Move flat-only block vectors/reservations/statistics to `FlatSceneStorage`.
6. Move data source, decoded cache, scheduler, requests, pins, and hierarchy error to
   `HierarchicalSceneStorage`.
7. Replace mode-error methods with explicit optional/query interfaces where callers
   legitimately accept either mode.
8. Preserve cancellation-and-wait destruction before moving callback ownership. Run
   under `tsan`.

#### Done when

Mode-specific invariants are enforced by storage type rather than scattered boolean
branches, and the facade remains behavior-compatible.

### 9.2 Extract scene snapshot and subscription management from the viewport

#### Why

`RenderViewportWidget` owns document revision tracking, scene subscriptions,
coalesced invalidation, cached scene snapshots, and layer retirement. These are
renderer-session responsibilities independent of mouse events and QRhi lifecycle.

#### How

1. Introduce `SceneSnapshotCache` owned by the viewport implementation.
2. Give it the `SceneDocumentSnapshot` from section 8.2 and an invalidation callback.
3. Let it maintain per-layer subscriptions and return current point-scene snapshots.
4. Move `knownLayerIds_`, `sceneSnapshots_`, `sceneSubscriptions_`, and wake
   coalescing into it.
5. Test add/remove/replacement, scene publication, stale callbacks, and coalesced
   wakeups without creating QRhi resources.

#### Done when

The widget receives a coherent render input snapshot and contains no subscription
maps.

### 9.3 Record golden frame plans before extracting coordinators

#### Why

Sections 9.4 and 9.5 move the selection, coverage, and residency logic that
contracts 8, 9, and 10 in section 3 declare inviolable. The only current evidence that
those contracts hold is the opt-in GPU lane and a manual qualification run. Extracting
first and porting tests afterwards means the tests are written to match whatever the
new code does.

`tests/qt/RenderSelectionTests.cpp` already exists, links `pcinspector::renderer`,
and after section 7.5 links only `renderer_planning`. Extend it now.

#### How

1. Do not make the test depend on `test_data/3445-343.laz` or
   `test_data/3445-344.laz`; `test_data/` is ignored and those files are local
   qualification inputs, not CI fixtures. Build deterministic synthetic flat and
   hierarchical scene snapshots whose topology and point totals exercise the same
   policies. Record the recipe and seed beside the fixtures.
2. Establish a characterization seam before redesigning ownership. Move the current
   frame-plan calculation body into a pure/internal free function with explicit
   inputs and outputs, delegate the widget to it unchanged, and drive that function
   with fake decoded/GPU residency queries. This mechanical extraction is part of
   the characterization commit, not the new coordinator design.
3. Drive the seam through a fixed sequence of camera poses and budgets, with no GPU.
4. Record the resulting selection as a golden fixture: selected block IDs and counts,
   per-layer coverage, upload and protection lists, and continuation flags.
5. Include the adversarial cases the contracts name: 25-layer fairness, a refinement
   step that must not open a coverage hole, insufficient CPU budget, insufficient GPU
   budget, and hidden layers.
6. Commit the small synthetic fixtures. Keep a separate same-host native
   qualification run over the two real files as a phase checkpoint; do not confuse
   that performance evidence with the portable golden test.

#### Done when

A change to selection or coverage policy fails a fast, GPU-free test before it
reaches the qualification run.

### 9.4 Extract full-detail coordination

#### Why

Finite local sources have a substantial warming and atomic-cutover state machine
inside the widget. It is correctness-sensitive but mostly independent of Qt input and
pass recording.

#### How

1. Move `FullDetailPlan`, per-layer decoded payloads, probe cursor, recovery attempts,
   admission checks, and progress state into `FullDetailController`.
2. Define inputs explicitly: document revision, visible layer descriptors, decoded
   budget, GPU budget, and residency queries.
3. Define outputs explicitly: node requests, payload leases, upload requests,
   drawable leaf blocks, warming progress, active state, and continuation need.
4. Preserve bounded probes per frame and atomic coarse-to-leaf cutover.
5. Add deterministic tests for complete working sets, insufficient CPU/GPU budgets,
   decode failure/recovery, hidden layers, and document revision changes.

#### Done when

Full-detail behavior is tested without `QRhiWidget`, the widget stores only the
controller instance, and section 9.3's fixtures still pass.

### 9.5 Extract point-frame coordination

#### Why

The viewport's frame planner currently combines frustum queries, global layer
allocation, hierarchy selection, decode requests, cache leases, upload requests,
GPU protection, full-detail handling, flat plan caching, and progress coverage.

#### Proposed output

```cpp
struct PointFramePlan {
    std::vector<SelectedBlock> blocks;
    std::vector<UploadBlock> uploads;
    std::vector<GpuBlockKey> protectedGpuBlocks;
    std::vector<PointCloudNodePayloadPtr> decodedLeases;
    std::unordered_set<SceneLayerId> outOfFrustumLayers;
    FrameCoverageMetrics coverage;
    bool requiresContinuation = false;
};
```

#### How

1. Move current private plan/value types to a portable or renderer-internal header.
2. Introduce `PointFrameCoordinator` owning hierarchy-selection history and flat-plan
   cache keys.
3. Inject narrow residency queries rather than the complete QRhi upload scheduler
   where practical.
4. Preserve this ordering:
   - refresh snapshots;
   - determine visible layers;
   - allocate fair coverage/detail budgets;
   - request/lease decoded nodes;
   - create upload/protection lists;
   - resolve drawable blocks;
   - publish coverage and continuation state.
5. Keep actual QRhi uploads and draw recording outside the coordinator.
6. Point section 9.3's golden fixtures at the new boundary; they must pass unchanged.

#### Done when

The widget asks one coordinator for a frame plan and no longer implements point LOD
or flat/hierarchical branching itself.

### 9.6 Extract measurement and telemetry

#### Why

Measurement hover scheduling/pick generations and diagnostic accumulation are two
additional independent state machines in the widget.

#### How

1. Move hover debounce, serials, stale-pick checks, anchor/preview/commit state, and
   distance creation into `MeasurementController`.
2. Keep Qt event translation in the widget; pass logical commands and pick results to
   the controller.
3. Move frame counter accumulation, saturation, sample windows, and publish timing
   into `RenderTelemetryAccumulator`.
4. Group the flat `RenderMetrics` internally into backend, frame, selection, upload,
   residency, decode, and picking submetrics. Preserve the existing external
   projection until qualification/report consumers migrate together —
   `pci_qualification_diff` and the stored baselines read that projection.
5. Test both controllers with deterministic time/input values.

#### Done when

The widget does not own measurement-generation counters or dozens of cumulative
metric fields.

### 9.7 Use RAII for QRhi resources

#### Why

Renderer classes manually delete buffers, pipelines, render targets, textures,
samplers, and bindings across success and exception paths. Ownership is clear enough
to encode directly.

#### How

1. Define narrow aliases such as `std::unique_ptr<QRhiBuffer>` where normal deletion
   is correct.
2. Use a custom deleter for QRhi objects requiring `release()` instead of `delete`.
3. Convert one renderer class at a time, beginning with leaf resource owners.
4. Preserve explicit destruction order where render targets refer to textures or
   render passes.
5. Keep all resource destruction on the current QRhi/render thread.
6. Delete now-unnecessary catch cleanup only after failure-path tests exist.

Do not add a general resource framework or GPU allocator in this refactor. Existing
qualification found no material buffer-creation bottleneck.

### Phase 4 checkpoint

Run:

```sh
cmake --build --preset development --parallel
ctest --preset development
cmake --build --preset asan-ubsan --parallel && ctest --preset asan-ubsan
cmake --build --preset tsan --parallel && ctest --preset tsan
cmake --build --preset native-gpu --parallel
ctest --preset native-gpu
```

Then reproduce the native qualification workload documented in
[`qualification/README.md`](../qualification/README.md) and compare with
`pci_qualification_diff` against a baseline captured on the same host at the start of
this phase. A non-zero exit is a blocking regression; section 13.3 describes how to
attribute it.

## 10. Phase 5: modernize import, jobs, persistence, and color-map ownership

### 10.1 Split point-load options, resources, and job context

#### Why

`PointCloudImportRequest` currently combines user configuration, memory/scheduler
resources, preflight results, cancellation, progress callbacks, and scene-ready
callbacks. Copies of the request therefore copy service handles and behavior as well
as data.

#### Proposed types

```cpp
struct PointCloudLoadOptions {
    std::filesystem::path sourcePath;
    std::uint64_t maximumPoints;
    LocalPagingOptions localPaging;
};

struct PointCloudLoadResources {
    std::uint64_t decodedByteBudget;
    HierarchyResidencyCoordinatorPtr residency;
    PointMemoryBudgetPtr memoryBudget;
    PointMemoryBudget::ReservationPtr flatReservation;
};

struct PointCloudLoadContext {
    std::stop_token stopToken;
    std::function<void(PointCloudImportProgress)> progress;
    std::function<void(PointCloudScenePtr)> sceneReady;
};
```

Note that `MainWindow`'s constructors default `decodedByteBudget` to
`PointCloudImportRequest::defaultDecodedByteBudget`; that constant needs a new home
before the request type is split.

#### How

1. Add aggregate construction helpers so call sites remain concise.
2. Change inspection to consume options plus only the resources it needs.
3. Return preflight as a value; pass it explicitly to load rather than placing it
   back into the request.
4. Pass cancellation and observers in the per-job context.
5. Keep memory reservations explicit and move-only where ownership is unique.
6. Update fake loaders and tests before production PDAL code.

#### Done when

Configuration values can be compared/serialized independently, resource ownership is
visible, and job callbacks are not embedded in reusable options.

### 10.2 Replace impossible outcome states with `std::expected`

#### Why

Controller outcomes currently contain an optional result, a string error, and a
cancellation boolean. More than one can be populated, even though only one terminal
state is valid.

#### Proposed contract

```cpp
enum class JobErrorCode {
    Cancelled,
    Validation,
    ResourceAdmission,
    Io,
    UnsupportedSource,
    Internal,
};

struct JobError {
    JobErrorCode code;
    std::string message;
};

template<class T>
using JobResult = std::expected<T, JobError>;
```

#### How

1. Translate current exception taxonomies at the worker boundary.
2. Return one expected value to the owner thread.
3. Map `Cancelled` to the cancelled job phase, known failures to failed, and values
   to success.
4. Keep domain exceptions inside deeply nested PDAL/OGR/local I/O where unwinding is
   useful; expected is the asynchronous and application boundary, not a mandate to
   remove all exceptions.
5. Convert CPT parsing to expected where it already models value or error without
   exceptions. CLI parsing already returns expected as of section 8.7.

#### Done when

Every asynchronous completion has exactly one terminal interpretation.

### 10.3 Adopt strong IDs subsystem by subsystem

#### Why

Layer IDs, point-source IDs, task IDs, and job IDs are all `uint64_t`, making
cross-domain mistakes compile. `LoadJobKey` compensates for this only in one area.

The `StrongId` template itself was added to foundation in section 7.2 and is already
in use for `SceneLayerId` from section 8.1. This step completes the adoption.

#### How

1. Define `PointCloudSourceId`, `TaskId`, and `LoadJobId` with distinct tags.
2. Keep `PointCloudLayerId` as an alias of `SceneLayerId` only while remaining
   point/vector callers migrate to the unified layer type.
3. Register Qt metatypes needed in queued connections and QVariant roles beyond the
   `SceneLayerId` registration from section 8.1.
4. Convert one subsystem at a time, starting at public boundaries and moving inward.
5. Serialize `.value()` explicitly in JSON and disk formats. Verify the qualification
   JSON still emits plain numbers — `pci_qualification_diff` and the stored baselines
   depend on it, and contract 3 forbids a schema change here.

#### Tests

Add compile-time non-convertibility assertions, hash/container tests, Qt metatype
round trips, and JSON numeric-output tests.

#### Done when

Passing a task, source, or job ID where a layer ID is required is a compile error.

### 10.4 Share asynchronous controller mechanics without merging workflows

#### Why

Point and vector controllers duplicate owner-thread posting, ID exhaustion checks,
terminal row retention, cancellation plumbing, and sorted state projection. Their
actual state machines differ and should remain explicit.

#### How

1. Extract a tested `postToObject(QPointer<T>, Callable)` Qt helper.
2. Extract strong job-ID allocation with overflow checks.
3. Extract small value helpers for terminal record retention and capabilities.
4. Keep `PointCloudLoadController::Job` and `VectorLoadController::Job` separate.
5. Do not introduce a virtual `ImportJob` hierarchy or a template state-machine
   framework.
6. Let `SceneSession` merge `jobRows()` and dispatch actions by strong/discriminated
   key; that facade already exists as of section 8.3.

#### Done when

Mechanically identical threading/identity code is shared, while point/vector phases
remain readable as concrete workflows.

### 10.5 Decompose `LocalPointIndexBuilder` behind its facade

#### Why

The builder's public API is appropriately small, but its 1,302-line implementation
combines several independently testable policies and resource lifecycles. Splitting
internal stages reduces the risk of changing cache validation while working on Morton
sorting or hierarchy sampling.

#### Target internals

- `LocalPageCacheLocator`: fingerprint, existing-entry lookup, and pruning.
- `LocalPageBuildLock`: inter-process ownership and stale-lock recovery.
- `MortonRunBuilder`: bounded source scan and sorted run creation.
- `LeafPageWriter`: merged leaf creation and payload checksums.
- `ParentHierarchyBuilder`: bounded bottom-up sampling.
- `LocalPageCommitter`: manifest-last write and atomic directory commit.
- `LocalPageBuildSession`: temporary-directory cleanup and live-source publication.

#### How

1. Add golden fixtures for manifest bytes and existing-store reopening. Contract 2
   has no byte-level test today, and this is the step that puts it at risk — land the
   fixtures before any extraction. Generate a **small, deterministic, committed v1
   store** from a tiny test source using the pre-refactor writer, then test that the
   post-refactor reader opens it. Keep the fixture under test data with its provenance
   and expected checksum. Do not use `qualification/page-cache/*.pcipages` as the
   automated fixture: that directory is ignored, is about 421 MiB on the review
   machine, and is not available to CI.
2. Extract RAII temporary-directory and lock types first.
3. Extract existing-entry validation and pruning without changing fingerprint inputs.
4. Extract run creation/reading helpers.
5. Extract leaf and parent builders using the current algorithms and limits.
6. Extract commit logic last, preserving manifest-last and winning-process behavior.
7. Keep `LocalPointIndexBuilder::openOrBuild()` as the orchestration facade.
8. After every extraction, run corruption, cancellation, race/reuse, disk-limit, and
   deterministic-store tests.

#### Done when

The facade reads as a sequence of named stages, each resource cleans itself up, and
v1 fixtures remain byte-compatible/readable.

### 10.6 Replace the global color-map catalog

#### Why

The catalog is process-global mutable state that becomes permanently frozen on its
first read. Correctness therefore depends on startup call order, and tests cannot
construct isolated catalogs.

#### Proposed lifecycle

1. Construct a mutable `PointColorMapCatalog` containing built-ins.
2. Register embedded CPT maps during bootstrap.
3. Validate collisions and return typed registration errors.
4. Call `freeze()` to obtain `shared_ptr<const PointColorMapCatalogSnapshot>`.
5. Inject the snapshot into point-color policy, UI selectors, legends, and
   `PointColorMapAtlas`.

#### How

1. Add instance-based catalog tests alongside existing global tests.
2. Change lookup/sampling helpers to accept a catalog snapshot.
3. Pass the catalog through application composition rather than a service locator.
4. Migrate UI and renderer together so IDs and atlas rows remain consistent.
5. Delete global registration and freeze-on-read state.

#### Done when

Catalog construction order is explicit, immutable consumers share one snapshot, and
tests can create independent catalogs.

### Phase 5 checkpoint

Run the full non-GPU, native-GPU, local-page, and qualification suites. The committed
small v1 fixture must reopen in CI. As an additional developer qualification on the
review host, reopen the ignored `qualification/page-cache` stores created before
this program and confirm they remain valid; their absence on another machine is not
a test failure.

## 11. Close-out: tooling and documentation

### 11.1 Promote static analysis and sanitizers to gating

The advisory `.clang-tidy` preset from section 6.9 and the sanitizer presets from
section 5.3 have been running throughout; the high-risk concurrency/lifetime subset
has already been required since the Phase 2 checkpoint. Now fix or explicitly
suppress the remaining findings, promote clang-tidy to gating, and broaden sanitizer
gating to every supported non-GPU test. Suppressions carry a comment explaining why.

### 11.2 Architecture documentation

Create `ARCHITECTURE.md` containing:

- the target dependency graph from section 7.1, kept in sync with the check in
  section 7.7;
- application/import/scene/render data flow;
- object ownership and destruction order;
- GUI, worker, and render thread rules;
- cache and residency ownership;
- instructions for adding a new import adapter, layer kind, or render pass.

Use short ADRs for durable choices, including the CI dependency-acquisition decision
from section 5.1 and the formatting decision from section 5.2. Archive the completed
agent implementation plans in `docs/superpowers/plans/` alongside the documents moved
in section 5.6, and correct stale `PointCloudDocument`, Metal-specific, and old
source-path references in whatever remains active.

## 12. Verification matrix

| Change area | Focused verification | Full suite | Native GPU | Qualification |
|---|---|---:|---:|---:|
| Phase 0 tooling | CI green, sanitizer baseline, diff-tool self-check | Yes | Yes | Yes (baseline) |
| Task UI cleanup | UI task-row tests | Yes | No | No |
| Constructor/service cleanup | Async + MainWindow tests | Yes | No | No |
| Document emptiness API | Scene-document + vector replace tests | Yes | No | No |
| Decoded cache consolidation | Hierarchy + residency bench | Yes | **Yes** | No |
| Hierarchy lifecycle cleanup | Hierarchy tests under TSan | Yes | No | No |
| Arithmetic/path/hash helpers | Unit + path tests | Yes | No | No |
| Target/file moves | Clean configure/build | Yes | No | No |
| Test-target split | Boundary check | Yes | No | No |
| QRhi header hiding | Renderer + renderer-internal tests | Yes | Yes | No |
| Layer model/snapshot | Scene + UI + renderer planning | Yes | Yes | Recommended |
| `SceneSession` extraction | Session + UI wiring tests | Yes | No | No |
| `MainWindow` shell reduction | UI action/shortcut/object-name tests | Yes | No | No |
| Dock split | UI dock characterization tests | Yes | No | No |
| Qt models | Model tests | Yes | No | No |
| `ApplicationConfig` | CLI table tests + smoke tests | Yes | Yes | No |
| Scene storage split | Scene tests under TSan | Yes | Recommended | No |
| Golden frame plans | Renderer-planning tests | Yes | No | Yes (record) |
| Viewport coordinator extraction | Renderer + golden plan tests | Yes | Yes | Yes |
| QRhi RAII conversion | Failure/resource GPU tests | Yes | Yes | Yes |
| Import API/results | Component + async tests | Yes | No | Recommended |
| Strong ID adoption | Compile-time + JSON output tests | Yes | No | Yes |
| Local builder split | All local-page tests + v1 golden fixtures | Yes | No | Yes |
| Color catalog injection | CPT + renderer atlas tests | Yes | Yes | Recommended |

Native qualification acceptance is decided by `pci_qualification_diff` against the
tolerance table in section 5.5, plus these properties that the tool cannot check:

- cache and GPU residency remain within configured budgets;
- every visible source retains coarse coverage during refinement;
- the final full-detail state matches the input workload's admission result;
- the viewport sleeps after settling;
- picking, EDL, vector overlays, and point-size behavior remain correct;
- qualification JSON remains valid under the existing schema.

Performance changes should be based on measurement. For example,
`UploadScheduler` currently builds and sorts eviction candidates repeatedly, and
decoded-cache trimming scans entries for an evictable oldest value. These may become
worth optimizing at larger working sets, but the stored qualification does not show
them as current bottlenecks. Add counters/benchmarks first, then choose an indexed
LRU or batch eviction plan only if the evidence supports it.

## 13. Safe stopping points and rollback

### 13.1 Where it is safe to stop

- **Phase 0, Phase 1, Phase 2 — net positive at every commit boundary.** Each commit
  deletes dead code, adds a gate, or moves a file without changing behavior. Stopping
  anywhere in this range leaves the codebase strictly better. If the program is
  time-constrained, this is the subset to run.
- **Phase 3 and Phase 4 — complete or revert.** From the first `SceneSession` commit
  until the dock split finishes, `MainWindow` is half-migrated and harder to reason
  about than either endpoint; the same holds for the viewport between sections 9.2
  and 9.6. Time-box each phase before starting it and land it on a branch so an
  abandoned attempt is a branch deletion rather than a revert series.
- **Phase 5 — commit-by-commit safe.** Its six sections are independent of one
  another; any prefix is a coherent stopping point.

### 13.2 Branch strategy

Phases 0–2 and Phase 5 may land directly on `main` given CI. Phases 3 and 4 each get
a long-lived branch that rebases on `main` and merges only at its checkpoint, so the
intermediate half-migrated states never appear in `main`'s history.

### 13.3 Attributing a qualification regression

`pci_qualification_diff` exits non-zero on regression, which makes it usable directly
as a bisect predicate:

```sh
git bisect start <bad> <good>
git bisect run tools/qualification-bisect.sh
```

where the script builds the `native-gpu` preset, runs the workload, and calls
`pci_qualification_diff` against the baseline captured at the start of the phase.
This is why section 5.5 comes before any restructuring: without it, "investigate
material regressions before continuing" has no method behind it, and a regression
found at the Phase 4 checkpoint would have to be located by reading nine commits.

## 14. Recommended commit sequence

Each commit is conceptually reversible and normally green. If a commit cannot be
described without "and," it is likely combining multiple steps and should be split.

Phase 0 — safety net:

1. Add CI running the development and ci presets.
2. Resolve the formatting decision; if adopting, land the repository-wide reformat.
3. Add ASan/UBSan and TSan presets and record baselines.
4. Add the compiler-version floor and `__cpp_lib_expected` check.
5. Add `pci_qualification_diff` with a committed tolerance table.
6. Restore `docs/` to version control.
7. Archive superseded root design documents.

Phase 1 — migration debt and primitives:

8. Add characterization tests for unified task rows.
9. Remove legacy task state and callbacks.
10. Add an `ImportServices` test construction helper.
11. Replace service-fabricating window/controller construction with
    executable-owned `ImportServices` injection.
12. Characterize vector-only document semantics.
13. Replace ambiguous document emptiness APIs.
14. Relocate decoded-cache metrics and the default byte budget.
15. Port cache tests to `DecodedPageCache` and delete `DecodedBlockCache`.
16. Remove unused hierarchy wrappers.
17. Add checked/saturating arithmetic and migrate metric callers.
18. Add hash combination and migrate composite keys.
19. Add Qt/filesystem path helpers and migrate callers.
20. Add the advisory clang-tidy preset and baseline.

Phase 2 — target boundaries:

21. Create foundation and move neutral geometry.
22. Add `StrongId` to foundation.
23. Extend `Bounds3d` and drop the vector-to-pointcloud dependency.
24. Split platform and navigation targets out of core.
25. Move `AdaptivePointBudget` to renderer planning.
26. Move the GPU point layout into the point-cloud target.
27. Move synthetic point generation out of production scene code.
28. Retire `pcinspector_core`.
29. Characterize the scheduler contract.
30. Move and rename the scheduler to tasking.
31. Create renderer-planning and move portable policy.
32. Move renderer value headers into renderer_api.
33. Hide QRhi concrete headers and split internal renderer tests.
34. Split test targets to match the target graph.
35. Add dependency-boundary checks.

Phase 3 — scene state and application orchestration:

36. Fix cross-kind layer ordering with tests.
37. Introduce unified `SceneLayer` storage.
38. Add and adopt `SceneDocumentSnapshot`.
39. Add `SceneSession` characterization tests.
40. Move point-load transactions and progress to `SceneSession`.
41. Move document commands and task projection to `SceneSession`.
42. Extract diagnostics, qualification, and workspace helpers from `MainWindow`.
43. Characterize the layer panel's dock behavior.
44. Split the Layers, Inspector, Tasks, and Diagnostics docks.
45. Introduce incremental layer and task models.
46. Introduce and test `ApplicationConfig`.
47. Extract bootstrap composition and simplify `main()`.

Phase 4 — render orchestration:

48. Split flat and hierarchical scene storage.
49. Extract scene snapshot and subscription caching from the viewport.
50. Record golden frame plans for the qualification workload.
51. Extract full-detail coordination.
52. Extract point-frame coordination.
53. Extract measurement control.
54. Extract render telemetry accumulation.
55. Convert QRhi resource owners to RAII, one class at a time.
56. Run native qualification and resolve regressions.

Phase 5 — import, jobs, persistence, color maps:

57. Split point import options, resources, and context.
58. Introduce expected-based job results.
59. Adopt strong IDs subsystem by subsystem.
60. Share owner-thread posting and job-mechanics helpers.
61. Decompose local index building behind its facade.
62. Replace global color-map state with an injected snapshot.

Close-out:

63. Promote sanitizers and clang-tidy to gating.
64. Write `ARCHITECTURE.md` and the ADRs.
65. Remove remaining temporary shims and rerun release qualification.

## 15. What not to introduce during this program

Avoid unrelated modernization for its own sake:

- Do not adopt C++ modules while Qt moc, private QRhi headers, PDAL/GDAL packages,
  and the supported compiler matrix are still header-oriented.
- Do not replace the tested task scheduler with coroutines without a concrete async
  composition problem that stop tokens and queued Qt completion cannot solve.
- Do not create a service locator or dependency-injection container; constructor
  composition is sufficient.
- Do not force point and vector jobs into a generic virtual state machine.
- Do not replace closed layer/storage alternatives with deep inheritance when
  `std::variant` expresses the model directly.
- Do not replace justified immutable `shared_ptr` leases with raw pointers merely to
  reduce shared-pointer usage.
- Do not create a generic cache framework until more than one active cache has the
  same policy and profiling shows the abstraction is useful.
- Do not rewrite QRhi rendering as native Metal/Vulkan/D3D during a maintainability
  refactor.
- Do not combine formatting, file movement, interface redesign, and behavior changes
  in one review.
- Do not add features. This program's output is structure; a feature landed midway
  through Phase 3 or Phase 4 makes the half-migrated state permanent.

## 16. Final completion criteria

The modernization program is complete when:

1. `MainWindow`, individual docks, `SceneSession`, `SceneDocument`, and the renderer
   coordinator each have one explainable responsibility.
2. Import, scene, renderer planning, QRhi, and application dependencies follow
   section 7.1's adjacency list and are automatically checked, in `src` and in tests.
3. One ordered, structurally immutable scene snapshot drives both UI and renderer
   membership.
4. Flat and hierarchical scene invariants have separate internal owners.
5. The viewport widget contains QRhi lifecycle and input translation rather than the
   entire render/residency application.
6. Point/vector jobs expose explicit configuration, resources, cancellation, and one
   typed result.
7. Local page-store code is staged internally while preserving its public facade and
   v1 disk compatibility, proven by golden fixtures.
8. Legacy task, cache, construction, and lifecycle paths have been deleted.
9. CI runs the non-GPU suite on every change; static analysis, sanitizers, and
   dependency checks gate rather than advise. Format checking also gates if Phase 0's
   formatting ADR adopted it; otherwise the ADR records formatting as explicitly out
   of scope for this program.
10. The complete CPU, UI, component, native-GPU, and qualification suites pass, with
    `pci_qualification_diff` exiting zero against a same-host baseline.

At that point future work — new layer types, attributes, filters, render passes, or
import adapters — has a clear extension point instead of another branch inside a god
object.

## 17. Revision notes

This revision responds to reviews of the 2026-07-31 draft. The overall direction was
confirmed against the codebase; the changes below correct factual details, ordering,
enforcement, and implementation gaps.

**Defects corrected**

1. `SceneSession` (then section 7.2) specified signals taking
   `SceneDocumentSnapshot`, a type introduced in section 8.2 — a phase later. The
   unified layer model and snapshot now open Phase 3 (sections 8.1–8.2), ahead of
   `SceneSession`.
2. The unified `SceneLayer` model (then section 8.1) used `StrongId`, introduced in
   section 9.3 — two phases later. `StrongId` now lands in foundation during Phase 2
   (section 7.2); section 10.3 covers adoption only.
3. The target graph placed `renderer_rhi` below `application`, implying a dependency
   that does not exist and that section 7.6 exists to prevent. They are siblings
   under the executable, and the graph is now stated as an adjacency list because
   section 7.7 turns it into a build rule.
4. Deleting `DecodedBlockCache` breaks `tests/gpu/PointPickerGpuTests.cpp:273`, which
   uses its default byte budget. Section 6.4 now lists all six consumers, splits the
   work into two commits, and the verification matrix marks it as requiring the GPU
   lane.
5. `AdaptivePointBudget` was to be moved into `navigation`, but its only consumer is
   `RenderViewportWidget`. It now goes to `renderer_planning` (section 7.3).
6. Section 4.5 adds the rule that prevents defects 1 and 2 from recurring, and
   section 16's verification checks it.
7. `/docs/` being ignored does not untrack the older files already in Git; it hides
   only newer untracked files. Section 5.6 now describes the actual mixed state and
   requires classification rather than blindly adding the directory.
8. Concrete PDAL/OGR adapters were incorrectly dependencies of both `application`
   and `import_async`. They now live at the executable composition root, while
   section 6.2 removes every constructor that instantiates them implicitly.
9. The proposed v1 page-store fixture pointed at an ignored, host-local 421 MiB
   qualification cache. Section 10.5 now requires a small committed v1 fixture and
   treats the large cache as optional developer qualification only.
10. The initial `SceneDocumentSnapshot` did not cover all reads the viewport makes
    from `SceneDocument`. Section 8.2 now maps membership, bounds, budgets, frustum
    indexing, point-scene publication, and aggregate metrics to explicit owners and
    forbids retaining a mutable document shortcut in the viewport.
11. `ApplicationConfig` represented parse errors but not successful `--help` and
    `--version` exits. Section 8.7 now models the complete invocation as a variant
    and keeps process termination outside the testable parser.

**Additions**

- Phase 0 (section 5) creates the safety net the draft assumed: CI, sanitizer
  presets, a toolchain floor, a qualification comparison tool with numeric
  thresholds, a resolved formatting decision, and archived stale documentation.
- Section 7.7 extends dependency enforcement to test targets, which currently link
  `app_model` and so compile against most of the tree.
- Section 8.5 enumerates the layer-panel behavior at risk and the docks' interfaces,
  matching the detail the draft gave the renderer controllers.
- Section 9.3 records golden frame plans before the Phase 4 extractions rather than
  porting tests afterwards.
- Section 13 adds safe stopping points, a branch strategy, and a bisect procedure.
- Section 1.1 states the program's scale honestly and names the Phase 0–2 subset as
  a valid smaller scope.
- Section 5.6 records the mixed documentation state caused by `.gitignore:74`: this
  guide and other newer files are ignored, older `docs/superpowers/` files remain
  tracked, and the four root documents to archive are tracked. The section fixes the
  ignore rule before any move and requires review of every newly visible file.

**Recalibrated**

- `ApplicationConfig` moved to the end of Phase 3 (section 8.7). `main.cpp` is 324
  lines; the justification is testability of a release contract, not file size.
- The commit sequence grew from 44 to 65 as a consequence of Phase 0 and of splitting
  commits that the draft's own "no *and*" rule would have rejected.
