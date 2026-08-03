# Point Cloud Inspector Codebase Review Recommendations

> Historical implementation plan. Superseded by
> `docs/superpowers/plans/2026-07-31-modernization-refactoring-guide.md` and the
> current `ARCHITECTURE.md`; retained here for design history.

Date: 2026-07-09

This document captures a senior C++ review of the current `pcinspector` codebase. It focuses on maintainability, clarity, extensibility, and performance. The project is currently in a good proof-of-concept state: the main seams are present, the tests cover important behavior, and the Metal path is functional. The recommendations below are about keeping the codebase scalable as more production features are added.

Validation update: the current debug build and all 16 CTest tests, including Metal smoke, point-picking, and LAS/LAZ/COPC fixture paths, pass. A rebuild with `-Wall -Wextra -Wpedantic` also produced no project compiler warnings. This is therefore a prioritization and maintainability review, not a list of known failing behavior.

Implementation update: the first `MetalRenderViewport` extraction is complete. GPU point-buffer uploads now live in `MetalUploadManager`, and camera-uniform resources, the point pipeline, and the main draw pass live in `MetalPointCloudRenderer`. The full test suite and warnings build passed after the change.

The most important theme is that several classes have grown from proof-of-concept glue into multi-responsibility components. That is expected at this stage, but the next features will become slower and riskier to implement unless the architecture is decomposed before more renderer, import, filtering, and coloring work lands.

## Executive Summary

### What is working well

- The project has useful boundaries between core logic, point-cloud data, import, application UI, and renderer abstractions.
- The `RenderViewport` interface gives a starting point for OS/backend-specific render paths.
- Navigation logic is mostly testable outside the renderer.
- PDAL loading is isolated behind a `PointCloudLoader` contract.
- The point format is compact: `GpuPoint` is 16 bytes, which is appropriate for high-throughput point rendering.
- The code has meaningful tests for import, navigation, renderer contracts, point picking, loading overlay behavior, and UI integration.
- The recent progress-bar changes correctly model the fact that "PDAL done" is not the same as "cloud visible".

### Main risks

- `MetalRenderViewport` has been reduced to QRhiWidget integration, renderer coordination, navigation/input, picking coordination, metrics, and callbacks. Further renderer work should extend the extracted components instead of returning upload or draw-path logic to the viewport.
- `MainWindow` contains enough progress and presentation state to justify a small, pure loading-progress model, but is still cohesive enough that a broad controller/presenter split is premature.
- The PDAL import path has avoidable memory pressure from sorting compound records and copying into final vectors.
- Shader and C++ enum values are manually coupled.
- Scalar color mapping uses fixed ranges instead of per-cloud/per-attribute ranges.
- The current data layout is adequate for known attributes but does not yet define how arbitrary future dimensions, filtering, and source-order mapping should work.
- CMake repeats source lists and target wiring, which will become more painful with more backends.
- GUI tests are useful but increasingly hard to read and diagnose.
- The adaptive budget draws the first `N` loaded points. The current lexicographic point order can therefore bias visible coverage toward a spatial subset of a large cloud.

## Recommended Priority Order

1. Next: extract the remaining renderer and app libraries in CMake, then link tests against them.
2. Add shared test utilities and split the asynchronous Metal picker scenario into named helpers.
3. Add import/render timing instrumentation and a representative benchmark path.
4. Verify adaptive-budget visual coverage with representative point orderings; do not rely on global first-`N` rendering as a future LOD strategy.
5. Add per-cloud scalar statistics and use them for color normalization.
6. Complete: extract `MetalUploadManager` and the main point draw path from `MetalRenderViewport`, keeping external behavior unchanged.
7. Measure and then refactor PDAL import memory movement, preserving explicit draw-to-source/attribute mappings where needed.
8. Add lightweight picking and coordinate-space helpers, plus thread/callback documentation.

Defer a general attribute-table abstraction, palette resources, typed error taxonomy, and backend-selection APIs until the associated product feature or second backend is underway.

## 1. Split `MetalRenderViewport`

### Status

Completed on 2026-07-09.

### Current state

`src/renderer/metal/MetalRenderViewport.cpp` now handles:

- QRhiWidget lifecycle.
- Camera uniform construction.
- Frame timing and adaptive point budget updates.
- Point picking submission and result resolution.
- Mouse and keyboard event handling.
- Renderer progress publication.
- Renderer failure handling.
- Smoke-test process exit behavior.

The viewport remains the renderer coordinator, but GPU resource ownership is no longer concentrated there:

- `MetalUploadManager`
  - validates point-buffer sizes;
  - owns buffer replacement and destruction;
  - performs chunked uploads for loaded and synthetic points;
  - reports upload progress.

- `MetalPointCloudRenderer`
  - owns the camera uniform buffer and shader bindings;
  - creates and recreates the main point pipeline;
  - updates the camera uniform before a pick pass;
  - records the main point draw pass.

### What should change

The first extraction is complete. Keep the renderer implementation split along these boundaries:

- `MetalRenderViewport`
  - Owns the `QRhiWidget` integration.
  - Receives Qt events.
  - Delegates rendering work.
  - Exposes the `RenderViewport` interface.

- `MetalPointCloudRenderer`
  - Owns point draw pipeline.
  - Records the main point draw pass.
  - Knows about `GpuPoint` vertex layout.

- `MetalUploadManager`
  - Creates/replaces point buffers.
  - Handles chunked uploads.
  - Emits upload progress.
  - Later becomes the place for staged/incremental upload.

Do not introduce a pipeline cache/factory, a separate input controller, or a catch-all render-session class in the first extraction. There is only one main point pipeline today, and the event mapping is still modest. Add those boundaries when new pipelines, input modes, or shared state make their ownership concrete.

### How to do it

The completed extraction preserved the existing render ordering: the camera uniform is updated before `MetalPointPicker` records its pass, so picks use the current camera state.

Validation completed:

- `cmake --build build --parallel 4`
- `ctest --test-dir build --output-on-failure` with all 16 tests passing
- warnings build with `-Wall -Wextra -Wpedantic`

Do not add a pipeline cache/factory, a separate input controller, or a catch-all render-session class until a concrete new pipeline, input mode, or shared state requires one.

### Why this matters

The renderer is about to gain:

- More color-map behavior.
- Filters.
- Attribute-driven rendering.
- Potential point-size modes.
- LOD and tiling.
- Backend-specific implementations for Metal, Vulkan, and likely D3D.

If all of that lands in `MetalRenderViewport`, the class will become difficult to reason about and risky to modify. Splitting now lowers the cost of future features.

### Recommended next step

Extract a `pcinspector_renderer_metal` CMake target that contains `MetalRenderViewport`, `MetalPointCloudRenderer`, `MetalUploadManager`, `MetalPointPicker`, and `ShaderLoader`. Link the application and renderer integration tests against that target instead of repeating the implementation files. This is the next smallest change with clear leverage: it makes the new renderer ownership boundary explicit in the build and removes source-list drift before the next renderer feature lands.

## 2. Refactor `MainWindow`

### Current state

`src/app/MainWindow.cpp` currently handles:

- File menu and toolbar construction.
- File dialog.
- Point-cloud loading workflow.
- Loading overlay state.
- Clock-weighted progress mapping.
- Renderer progress mapping.
- Color source selector.
- Color map selector.
- Renderer metrics display.
- Renderer failure display.
- Load failure dialogs.

The class is understandable and still has a coherent ownership boundary. Its progress calculation is the one concern that is both independently testable and likely to grow.

### What should change

Extract a pure `LoadingProgressModel` or `LoadingProgressPresenter` that maps import/render events and timer updates to a monotonic percentage plus display detail. Keep the actions, color controls, and status-bar formatting in `MainWindow` until they become independently complex or reused.

### How to do it

Suggested extraction order:

1. Extract progress constants and progress update methods into a pure model/presenter.
2. Add phase-transition and monotonicity tests around that model.
3. Keep `MainWindow` as the owner of widgets, actions, and workflow-level connections.

Each step should keep object names stable because tests depend on them:

- `openPointCloudAction`
- `pointCloudToolBar`
- `colorSourceComboBox`
- `colorMapComboBox`
- `loadingOverlay`
- `loadingProgressBar`
- `loadingDetailsLabel`
- `loadingCancelButton`

### Why this matters

This isolates the part with meaningful state and timing behavior without creating controller classes whose only responsibility is to wrap a few widget connections.

## 3. Reduce PDAL Import Memory Pressure

### Current state

`PdalPointCloudLoader` streams points from PDAL, creates `LoadedPointRecord` values, sorts the records, then copies data into final `LoadedPointCloud` vectors.

The current intermediate record contains:

- `GpuPoint point`
- `PointAttributes attributes`

This is simple and correct, but it increases memory bandwidth and peak memory usage.

### What should change

Refactor import to reduce copies and avoid sorting larger records than necessary.

Preferred near-term options, to be selected only after a representative benchmark:

1. Store final vectors directly, then sort indices.
2. Sort only `GpuPoint` if CPU attributes are not required in draw order.
3. Use a structure-of-arrays layout for CPU attributes and reorder only necessary arrays.
4. If draw order is intended to become spatial ordering, move toward chunked/tiled ordering instead of one global sort.

### How to do it

A practical first experiment:

1. Stream PDAL points into:
   - `std::vector<GpuPoint> points`
   - `std::vector<PointAttributes> attributes`
2. Build `std::vector<std::uint32_t> order`.
3. Sort `order` by comparing `points[index]`.
4. Apply the permutation in place or release the original storage before allocating replacement storage.
5. Measure before/after import time, peak memory, and draw coverage.

Sorting a small index vector reduces comparison bandwidth, but an out-of-place permutation can increase peak memory. Do not assume this refactor is a memory win without measuring it.

If the CPU attributes do not need to be in draw order, do not reorder them directly. Instead, introduce an explicit mapping:

- `drawIndexToSourceIndex`
- or `drawIndexToAttributeIndex`

This will also help picking because a picked draw index can resolve back to source metadata.

### Why this matters

Point-cloud files are large. For 10 million points:

- `GpuPoint` alone is roughly 160 MB.
- Intermediate records add more memory.
- Sorting large records is bandwidth-heavy.
- Copying after sort adds another pass over memory.

Reducing memory movement is likely to improve import time and make larger files possible before out-of-core loading exists.

## 4. Define a Clear Point-Cloud Data Layout Strategy

### Current state

`LoadedPointCloud` stores:

- `PointCloudMetadata metadata`
- `std::vector<GpuPoint> points`
- `std::vector<PointAttributes> attributes`
- decode center/extent

`GpuPoint` is compact and GPU-friendly. `PointAttributes` stores the known LAS properties needed soon for coloring/filtering.

### What should change

Document the intended data model now, but do not introduce a general attribute-table API until arbitrary dimensions, filtering, or point inspection are committed features:

- GPU payload:
  - compact position
  - packed RGB
  - packed high-frequency attributes used by shaders

- CPU attribute columns:
  - typed, named columns for filtering/coloring/picking details
  - one vector per attribute, or a column store abstraction

- Metadata/statistics:
  - min/max for numeric attributes
  - observed categorical values
  - source dimensions
  - source bounds
  - source scale/offset when needed

- Mappings:
  - draw index to source ordinal
  - draw index to attribute row
  - future tile/chunk id

### How to do it

The first concrete addition should be an explicit draw-index-to-source-ordinal/attribute-row mapping for picking. Introduce a `PointAttributeTable` only when a real consumer needs arbitrary attributes. Keep its ownership and core dependencies explicit: an owning typed-vector implementation using standard-library names is preferable to a `QString` plus non-owning `std::span<const std::byte>` public representation.

One possible later shape is:

```cpp
enum class PointAttributeType {
    UInt8,
    UInt16,
    UInt32,
    Float32,
    Float64,
};

struct PointAttributeColumn {
    std::string name;
    PointAttributeType type;
    std::variant<
        std::vector<std::uint8_t>,
        std::vector<std::uint16_t>,
        std::vector<std::uint32_t>,
        std::vector<float>,
        std::vector<double>> values;
    double minimum;
    double maximum;
};
```

The key is that consumers should not need to know whether an attribute came from LAS, LAZ, COPC, or a synthetic source.

### Why this matters

The next feature set will need:

- Coloring by arbitrary property.
- Filtering by arbitrary property.
- Displaying picked point details.
- Preserving source metadata.
- Potentially writing selections or filtered outputs later.

A clear attribute model avoids repeatedly extending `GpuPoint` or adding one-off fields for every LAS dimension.

## 5. Move Scalar Color Ranges to Metadata

### Current state

The renderer currently passes fixed scalar ranges to the shader. This works for normalized coordinates and full-range intensity, but it is not generally correct.

For example:

- A file may only use intensity values from `1000..2500`.
- Elevation may be a narrow slice of the normalized volume.
- Future dimensions may have arbitrary ranges.
- Good color mapping often needs percentile clipping, not raw min/max.

### What should change

Add per-source scalar range metadata:

```cpp
struct ScalarRange {
    double minimum = 0.0;
    double maximum = 1.0;
};

struct PointColorStatistics {
    ScalarRange x;
    ScalarRange y;
    ScalarRange z;
    std::optional<ScalarRange> intensity;
    std::unordered_map<std::string, ScalarRange> numericAttributes;
};
```

Use this to populate `CameraUniform::scalarMinimum` and `CameraUniform::scalarMaximum` based on the selected color source.

### How to do it

1. Derive X/Y/Z ranges from source bounds and the chosen GPU encoding; collect intensity and future attribute ranges while streaming the loaded sample.
2. Store ranges on `LoadedPointCloud` or `PointCloudMetadata`.
3. Add a helper:

```cpp
ScalarRange scalarRangeForColorSource(
    const LoadedPointCloud& cloud,
    PointColorSource source);
```

4. Update `MetalRenderViewport::cameraUniform()` to select the active range.
5. Add tests for intensity range and Z range.

### Why this matters

Color maps are only useful if the normalization is meaningful. Fixed `0..65535` makes many real datasets look low-contrast. Per-cloud ranges improve usability without changing the GPU point layout.

## 6. Formalize Shader and C++ Contracts

### Current state

Shader constants duplicate C++ enum integer values:

- `PointColorSource`
- `PointColorMap`
- shader `ColorRgb`, `ColorX`, `MapViridis`, etc.

This is acceptable for a proof of concept, but it is fragile.

### What should change

Create a single source of truth for enum integer values used across C++ and shaders. This is a useful guardrail, but should follow the CMake/test cleanup rather than block the first renderer extractions.

Options:

1. Generate a small shader include from C++ metadata or a neutral schema.
2. Define the enum values in a small `.json`/`.toml` contract and generate both C++ and shader constants.
3. Keep manual duplication temporarily, but have a contract test read the GLSL source as well as assert the C++ values.

A C++-only test of expected enum values cannot detect a changed shader constant. Prefer option 1 or 2 when code generation is already justified; otherwise use option 3 as a temporary source-level test.

### How to do it

Add a `renderer_shader_contract_tests.cpp` test that asserts the C++ values and checks the matching constants in `points.vert`:

```cpp
static_cast<int>(PointColorSource::Rgb) == 0;
static_cast<int>(PointColorSource::X) == 1;
static_cast<int>(PointColorSource::Y) == 2;
static_cast<int>(PointColorSource::Z) == 3;
static_cast<int>(PointColorSource::Intensity) == 4;
static_cast<int>(PointColorSource::Classification) == 5;
static_cast<int>(PointColorSource::ReturnNumber) == 6;
static_cast<int>(PointColorSource::NumberOfReturns) == 7;

static_cast<int>(PointColorMap::Rgb) == 0;
static_cast<int>(PointColorMap::Grayscale) == 1;
static_cast<int>(PointColorMap::Viridis) == 2;
static_cast<int>(PointColorMap::Turbo) == 3;
static_cast<int>(PointColorMap::LasClassification) == 4;
static_cast<int>(PointColorMap::ReturnNumber) == 5;
```

Do not treat the C++ assertions alone as proof of the shader contract.

### Why this matters

If a future enum change silently desynchronizes shader and C++ values, rendering will produce wrong colors without obvious compile errors. A small contract test prevents this class of bug.

## 7. Prepare for Palette-Based Color Maps Later

### Current state

The first color-map implementation uses hard-coded shader functions for continuous maps and hard-coded switch statements for categorical maps.

That is acceptable for the first version. It avoids extra GPU resources and keeps color mode changes cheap.

### What should change later

Move to palette-driven maps once one of these becomes true:

- user-defined color maps are needed;
- exact Viridis/Turbo visual fidelity matters;
- categorical maps need more classes or configurable colors;
- CPU/UI needs to display matching legends;
- color-map definitions should be shared across Metal/Vulkan/D3D.

### How to do it

Future direction:

- Store predefined color maps as app assets.
- Use a small 1D texture or uniform array palette.
- For categorical maps, use integer lookup into a palette.
- For continuous maps, normalize scalar to `0..1` and sample palette.
- Keep `PointColorMap` as the user-facing enum but map it to palette resources internally.

### Why this matters

Hard-coded shader color functions are fast and simple, but they do not scale well as maps become data-driven. Palette resources will make legends, user customization, and cross-backend parity easier.

## 8. Improve Loading Progress Architecture

### Current state

Progress is split across:

- PDAL import progress.
- estimated optimizing progress.
- renderer preparing progress.
- GPU upload progress.
- first-frame-ready completion.

The progress bar now waits until the first frame is ready before reaching 100%, which is the right user-facing behavior.

### What should change

Make progress phases explicit data instead of constants spread through `MainWindow`.

Example:

```cpp
struct LoadingPhaseRange {
    int startPercent;
    int endPercent;
    std::chrono::milliseconds estimatedDuration;
    QString label;
};
```

Then define:

- Reading: `0..33`
- Optimizing: `33..80`
- Renderer preparing/uploading: `80..98`
- First frame ready: `100`

### How to do it

Move progress calculation into `LoadingProgressPresenter`:

- `onImportProgress(stage, processed, total)`
- `onRenderProgress(stage, completed, total)`
- `onTimerTick()`
- `complete()`
- `cancel()`
- `fail()`

Add tests around phase transitions and monotonicity.

### Why this matters

Progress behavior is user-visible and performance-sensitive. Keeping it explicit prevents future regressions where the bar reaches 100% before the point cloud is visible.

## 9. Add Benchmark and Timing Infrastructure

### Current state

The renderer reports frame metrics. The load pipeline has user-facing progress but not enough structured timing data for performance analysis.

### What should change

Add repeatable benchmarks and timing markers:

- PDAL inspection time.
- PDAL stream/read time.
- point conversion/packing time.
- sort/reorder time.
- GPU buffer creation time.
- GPU upload time.
- first visible frame time.
- steady-state frame time at fixed point budgets.
- point picking latency.

### How to do it

Add a small internal timing utility:

```cpp
struct TimedStage {
    QString name;
    std::chrono::steady_clock::duration duration;
};

struct LoadTimingReport {
    std::vector<TimedStage> stages;
};
```

Expose timing in smoke-test mode and optionally in debug logs. Add command-line options:

- `--benchmark-load`
- `--benchmark-render`
- `--benchmark-json <path>`

For now, a simple JSON output file is enough.

### Why this matters

This app is fundamentally performance-driven. Without baseline timings, it will be hard to tell whether changes improve or regress behavior. Benchmarks also help decide whether a refactor is actually worth it.

## 10. Improve CMake Structure

### Current state

`CMakeLists.txt` already has reusable `pcinspector_core`, `pcinspector_pointcloud`, `pcinspector_import_pdal`, and `pcinspector_import_async` targets. It still directly lists app and Metal renderer sources in the executable and repeats the Metal source lists in multiple tests.

This is manageable now but will scale poorly with more renderer backends and more tests.

### What should change

Create the remaining reusable internal libraries:

- `pcinspector_app`
- `pcinspector_renderer_api`
- `pcinspector_renderer_metal`

Tests should link these libraries instead of repeating implementation files.

### How to do it

Suggested structure:

```cmake
add_library(pcinspector_renderer_metal
    src/renderer/metal/MetalRenderViewport.cpp
    src/renderer/metal/MetalPointPicker.cpp
    src/renderer/metal/ShaderLoader.cpp)

target_link_libraries(pcinspector_renderer_metal
    PUBLIC pcinspector_renderer_api
    PRIVATE pcinspector_core pcinspector_pointcloud Qt6::Gui Qt6::GuiPrivate Qt6::Widgets)
```

Then tests can do:

```cmake
target_link_libraries(pcinspector_renderer_contract_tests
    PRIVATE pcinspector_renderer_metal Qt6::Test)
```

Also add helper CMake functions:

```cmake
function(pcinspector_add_test name)
    add_executable(pcinspector_${name}_tests ${ARGN})
    add_test(NAME ${name} COMMAND pcinspector_${name}_tests)
endfunction()
```

### Why this matters

Duplicated Metal source lists already make ownership and test linkage harder to change. Extracting the remaining libraries now makes future backend work safer without waiting for a second backend.

## 11. Add Project-Wide Compiler Warnings and Static Checks

### Current state

The CMake file sets C++23 and Qt settings but does not appear to define a project warnings/options target.

### What should change

Add a `pcinspector_project_options` or `pcinspector_warnings` interface target.

Recommended warnings for Clang/GCC:

- `-Wall`
- `-Wextra`
- `-Wpedantic`
- `-Wconversion` after initial cleanup, not immediately if it causes too much noise
- `-Wshadow`
- `-Wnon-virtual-dtor`
- `-Wold-style-cast`

For MSVC later:

- `/W4`
- targeted disables only when necessary

### How to do it

Add:

```cmake
add_library(pcinspector_project_options INTERFACE)
target_compile_features(pcinspector_project_options INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(pcinspector_project_options INTERFACE /W4)
else()
    target_compile_options(pcinspector_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast)
endif()
```

Link it to internal targets.

### Why this matters

The code uses a lot of numeric conversions and GPU-facing binary layouts. Compiler warnings are a cheap way to catch mistakes early.

## 12. Improve GUI Test Readability

### Current state

The current tests use custom `CHECK` macros and direct event-loop polling. This keeps dependencies low, but some tests are now difficult to read. The point picker test in particular has deeply nested timers and lambdas.

### What should change

Create shared test utilities and split long GUI scenarios.

Recommended utilities:

- `tests/TestChecks.h`
- `tests/QtTestUtils.h`
- `processUntil(predicate, timeout)`
- `sendMouseDrag(...)`
- `sendWheel(...)`
- `sendKeyPress(...)`
- `makeSinglePointCloud(...)`

### How to do it

First step:

1. Extract duplicated `CHECK` and `CHECK_NEAR`.
2. Extract `processUntil`.
3. Rewrite `metal_point_picker_tests.cpp` into named helper functions:
   - `waitForCenterPick`
   - `waitForMiss`
   - `performWheelZoom`
   - `performPivotPick`
   - `performKeyboardMove`

Do not switch the entire suite to QtTest in one pass. Use QtTest where it materially improves diagnostics.

### Why this matters

Tests are only useful if failures are easy to understand. Nested timer tests tend to fail with opaque timeout codes. Better test helpers will make renderer regressions faster to diagnose.

## 13. Strengthen Renderer Backend Abstractions

### Current state

`RenderViewport` abstracts the current renderer enough for the app to use it. The concrete implementation is currently Metal-focused through `QRhiWidget`.

### What should change

Keep the existing `RenderViewport` boundary and ensure app code remains free of Metal includes. Do not add a `RenderBackend` enum, capability structure, or backend-selection UI until a second backend is actively being implemented.

At that point, clarify the backend model:

- macOS: QRhiWidget + Metal.
- Windows: likely QRhiWidget + D3D/Vulkan.
- Linux: likely QRhiWidget + Vulkan.

Avoid app-level assumptions about Metal. Keep backend-specific details behind renderer factories and capability queries.

### How to do it

When the second backend begins, extend the renderer API with capabilities such as:

```cpp
struct RenderBackendCapabilities {
    bool supportsIntegerVertexAttributes = false;
    bool supportsPointSizeInVertexShader = false;
    bool supportsGpuPicking = false;
    QString backendName;
};
```

Add backend selection only when there is a supported alternative:

```cpp
enum class RenderBackend {
    Auto,
    Metal,
    Vulkan,
    D3D,
};
```

Update `createRenderViewport()` to accept backend selection and platform defaults.

### Why this matters

The current renderer is deliberately Metal-only, while `MainWindow` already depends only on `RenderViewport`. Premature selection APIs would add unimplemented surface without reducing present coupling.

## 14. Improve Point Picking Architecture

### Current state

`MetalPointPicker` renders a 5x5 readback region and decodes the nearest ID. This is reasonable and efficient enough for the first version.

### What should change

Keep the picking implementation backend-specific, but make the pick request/result model backend-neutral.

Add:

```cpp
struct PickResult {
    std::uint32_t drawIndex;
    std::uint64_t sourceOrdinal;
    Vec3d normalizedPosition;
    std::array<double, 3> sourcePosition;
};
```

The source ordinal and source-space position should come from the point-cloud data model, not the renderer.

### How to do it

1. Keep `MetalPointPicker` returning draw index.
2. Add a resolver outside `MetalPointPicker`:
   - draw index -> `GpuPoint`
   - draw index -> source/attribute row
   - normalized position -> source-space position using `decodeCenter` and `decodeExtent`
3. Use `PickResult` for navigation and future UI details.

### Why this matters

Picking will eventually power:

- zoom-to-point;
- pivot selection;
- point inspection;
- filtering by picked class/property;
- measurement tools.

Those features need source-space information, not just a GPU draw index.

## 15. Make Navigation Units Explicit

### Current state

Navigation operates in normalized point-cloud coordinates. The camera defaults to Z-up and supports free movement.

This is good for rendering, but the distinction between normalized space and source space is implicit.

### What should change

Make coordinate spaces explicit in names and helper functions:

- `NormalizedPoint`
- `SourcePointPosition`
- `decodeNormalizedPosition(...)`
- `encodeSourcePosition(...)`

### How to do it

Add small wrappers or naming conventions first. Avoid heavy type machinery unless bugs appear.

Example:

```cpp
Vec3d decodeGpuPointToNormalized(const GpuPoint& point);
std::array<double, 3> decodeNormalizedToSource(
    Vec3d normalized,
    const LoadedPointCloud& cloud);
```

Use these helpers in:

- renderer picking;
- future measurement tools;
- point details UI;
- tests.

### Why this matters

The app must eventually report real-world coordinates. Making coordinate spaces explicit prevents subtle bugs in picking, navigation, and display.

## 16. Review Adaptive Point Budget Behavior

### Current state

`AdaptivePointBudget` adjusts active point count based on frame time. It is simple and testable.

### What should change

Treat representative coverage as a near-term correctness check, not an eventual optimization. The current loader uses lexicographic X/Y/Z ordering and the renderer draws the first `N` points, so large clouds can be visibly biased toward a spatial subset.

### How to do it

Short term:

- Add metrics showing active point ratio.
- Verify coverage visually and quantitatively with source, lexicographic, random, and spatially coherent orderings.
- Keep the current budget only if those checks demonstrate acceptable coverage for the supported dataset sizes.

Medium term:

- Use chunk/tile-level budgeting.
- Draw a subset of each tile instead of the first global N points.
- Add LOD levels or precomputed representative samples.

### Why this matters

Adaptive budgets should preserve visual coverage while reducing work. A global first-N strategy may improve FPS while making the visible cloud misleading.

## 17. Add Import and Render Error Taxonomy

### Current state

Errors are mostly plain strings. This is acceptable for the proof of concept and should remain so until the UI needs distinct retry/fallback behavior or diagnostics collection.

### What should change

When that need arises, introduce typed error categories:

- unsupported file type;
- invalid metadata;
- empty point cloud;
- non-streamable PDAL pipeline;
- missing backend capability;
- GPU resource creation failure;
- shader load failure;
- cancellation.

### How to do it

Add lightweight error codes:

```cpp
enum class PointCloudImportErrorCode {
    UnsupportedFileType,
    InvalidMetadata,
    EmptySource,
    NonStreamablePipeline,
    PdalFailure,
};
```

Keep user-facing messages, but preserve structured codes internally.

### Why this matters

Structured errors make it easier to:

- show better dialogs;
- write precise tests;
- decide retry/fallback behavior;
- collect diagnostics from users.

## 18. Keep the 16-Byte `GpuPoint`, But Plan Versioning

### Current state

`GpuPoint` is 16 bytes:

- 3x uint16 normalized coordinates
- packed attributes
- RGBA
- packed properties

This is a good compact format for throughput.

### What should change

Keep this layout for now. The existing size assertion is valuable; document the shader vertex layout before adding fields. A named `GpuPointV2` is only needed when a real incompatible vertex layout is introduced.

Possible future options, when an incompatible layout is needed:

- `GpuPointV1`: current 16-byte layout.
- `GpuPointV2`: optional additional attribute buffer instead of expanding the vertex.
- Separate attribute textures/buffers for large attribute sets.

### How to do it

Add comments and static assertions:

```cpp
static_assert(sizeof(GpuPoint) == 16);
static_assert(alignof(GpuPoint) <= 4);
```

Document the shader vertex layout next to the struct:

- location 0: position + packed attributes
- location 1: RGBA
- location 2: packed properties

### Why this matters

The point format is on the renderer hot path. Accidental growth from 16 to 20 or 24 bytes would directly reduce memory bandwidth efficiency.

## 19. Make Loading and Rendering Thread Ownership Explicit

### Current state

`PointCloudLoadController` correctly uses queued invocation for progress and `QtConcurrent::run` for loading. Renderer callbacks are deferred via `QTimer::singleShot` in key places to avoid recursive frame problems.

### What should change

Document thread ownership and callback rules:

- import callbacks may originate from worker threads;
- UI updates must be queued to the GUI thread;
- renderer callbacks from within a QRhi frame should be deferred;
- no `repaint()` or direct frame-triggering calls from renderer callbacks.

### How to do it

Add a short `docs/threading-and-callback-rules.md` or include a section in renderer docs.

Add comments near:

- `PointCloudLoadController::load`
- `MetalRenderViewport::publishMetrics`
- `MetalRenderViewport::publishLoadProgress`

### Why this matters

The app already hit a recursive repaint / active frame crash. Threading and callback rules should be explicit so future changes do not reintroduce that class of bug.

## 20. Revised Near-Term Implementation Plan

### Phase A: Build, tests, and measurement

Goal: reduce duplication and establish a performance baseline without changing behavior.

Tasks:

1. Next: extract `pcinspector_renderer_metal` and `pcinspector_app` targets, preserving existing core/import libraries.
2. Link tests against those targets instead of repeating Metal and app implementation sources.
3. Add shared assertion/event-loop helpers and split the Metal picker test into named steps.
4. Add staged load timing, fixed-budget render timing, first-visible-frame timing, and pick latency output.
5. Add a source-level shader/C++ contract test, or generate shared constants if code generation is justified.

Validation:

- Full build and full test suite.
- Smoke tests with synthetic, LAS, LAZ, and COPC fixtures.
- A benchmark result for a representative `.laz` that can serve as a before/after baseline.

### Phase B: Visual correctness and scalar ranges

Goal: avoid regressions in what the user sees before optimizing the render path.

Tasks:

1. Test visual coverage for the first-`N` adaptive budget under source, lexicographic, random, and spatially coherent orderings.
2. Define the next ordering/LOD step based on that result; do not assume global sorting provides representative coverage.
3. Add scalar statistics for the loaded cloud and select the active range in `CameraUniform`.
4. Add intensity- and Z-range tests plus a manual visual check on a narrow-range dataset.

Validation:

- Coverage remains representative at reduced point budgets.
- Scalar maps use the intended range and do not regress RGB or categorical modes.

### Phase C: Focused renderer and import changes

Goal: lower the cost of planned features while proving performance benefits.

Tasks:

1. Complete: extract `MetalUploadManager` and the main point draw path from `MetalRenderViewport` in separate mechanical changes.
2. Keep input mapping in the viewport unless it gains a second consumer or materially grows.
3. Measure the current import path, then choose a reorder design that reduces comparison/memory movement without increasing peak memory.
4. Preserve or add draw-index-to-source/attribute mappings whenever reorder semantics change.
5. Add a backend-neutral pick-result resolver and explicit normalized/source coordinate helpers.

Validation:

- Renderer contract, picker, main-window, and PDAL import tests remain green after every extraction.
- Benchmark results show the expected import or render benefit before retaining a more complex data layout.

### Phase D: Defer Until a Concrete Consumer

Start these only when their triggering feature is underway:

1. General attribute tables: arbitrary dimension coloring, filtering, or point-inspection UI.
2. Palette resources: configurable maps, exact palette requirements, or matching legends.
3. Typed error codes: retry/fallback behavior or diagnostic collection.
4. Backend selection/capabilities: implementation of a supported second backend.

Document thread/callback ownership and the 16-byte GPU vertex layout independently; both are low-risk maintenance work and do not require a broad architecture change.

## Final Notes

The current code is not in a bad state. It is in the normal transition point between "proof of concept that works" and "system that needs architecture to keep moving quickly." The most important decision is to address measured visual/performance risks and extract concrete responsibilities from `MetalRenderViewport`, rather than introducing broad abstractions ahead of their consumers.

The best next engineering move is not a rewrite. It is a sequence of small extractions backed by the existing tests, followed by benchmark instrumentation so performance decisions are based on measurements rather than assumptions.
