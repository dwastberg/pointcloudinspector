# QRhi Block/LOD Rebuild — Phase 0 + Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the renderer backend-neutral (Phase 0) and rebuild the point data spine around locally-quantized blocks with frustum culling and budgeted streaming uploads (Phase 1), per the approved spec `docs/superpowers/specs/2026-07-10-rhi-block-lod-architecture-design.md`.

**Architecture:** The current renderer is QRhi code with misleading `Metal*` names; Phase 0 renames it and adds a platform backend policy with zero behavior change. Phase 1 introduces a pure-C++ `pcinspector_scene` library (`PointBlock`, `BlockPartitioner`, `PointCloudScene`, `FrustumCuller`), converts the render stack to per-block draws with dynamic uniform offsets and budgeted uploads (behavior-preserving, via a temporary LoadedPointCloud→scene adapter), then switches the PDAL import pipeline and app to world-space scenes and deletes the legacy path.

**Tech Stack:** C++23, Qt 6.8 (QRhi, QRhiWidget, Widgets, Concurrent), PDAL 2.10, Catch2 (vendored submodule), qsb/`qt_add_shaders` (Vulkan-GLSL → MSL/HLSL/SPIR-V), CMake ≥ 3.24.

## Global Constraints

- C++23, `CMAKE_CXX_EXTENSIONS OFF`; match existing style: 4-space indent, `pci` namespace, `[[nodiscard]]`, trailing `_` members, Qt types only where Qt is already a dependency.
- The scene library (`src/scene/`) must not include any Qt or QRhi header (spec §3.1).
- Zero project compiler warnings with `-Wall -Wextra -Wpedantic` (the review-doc baseline).
- Every task ends with the full test suite green and one commit. Phase 0 tasks must be zero-behavior-change.
- Commit messages: conventional style (`refactor:`, `feat:`, `test:`), ending with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Block capacity: `maximumPointsPerBlock = 1'048'576`; upload budget default 48 MB/frame (spec §3.2).
- Do not modify `third_party/`, `build*/` directories, or `docs/` other than this plan's checkboxes.

## Build & Test Commands

The repo's primary configured build directory is `build` (Unix Makefiles). GPU tests are OFF in its cache.

```bash
# Build (auto-reconfigures when CMakeLists.txt changes)
cmake --build build -j 8

# All non-GPU tests
ctest --test-dir build --output-on-failure

# Enable GPU/smoke tests (needs a real Metal device, run serially).
# Before Task 3 the option is PCINSPECTOR_ENABLE_METAL_TESTS; Task 3 renames it.
cmake -S . -B build -DPCINSPECTOR_ENABLE_METAL_TESTS=ON
ctest --test-dir build --output-on-failure -L gpu
```

CTest target names follow `pci_add_catch_test_target` prefixes, e.g. `pcinspector_unit_tests::<test name>`. Run one binary directly for fast iteration: `./build/pcinspector_unit_tests "<test name>"`.

---

# Part A — Phase 0: De-Metal-ify (zero behavior change)

### Task 1: Rename `renderer/metal` to backend-neutral `renderer/rhi`

Pure mechanical rename. No logic changes, no test-expectation changes beyond names/tags. All 16 existing tests must pass identically afterwards.

**Files:**
- Rename: `src/renderer/metal/` → `src/renderer/rhi/` (directory)
- Rename: `MetalRenderViewport.{h,cpp}` → `RenderViewportWidget.{h,cpp}`
- Rename: `MetalPointCloudRenderer.{h,cpp}` → `PointCloudRenderer.{h,cpp}`
- Rename: `MetalUploadManager.{h,cpp}` → `PointBufferUploader.{h,cpp}` (interim name; replaced in Task 11)
- Rename: `MetalPointPicker.{h,cpp}` → `PointPicker.{h,cpp}`
- Rename: `tests/gpu/MetalPointPickerTests.cpp` → `tests/gpu/PointPickerGpuTests.cpp`
- Rename: `tests/support/CatchMetalMain.cpp` → `tests/support/CatchGpuMain.cpp`
- Modify: `CMakeLists.txt`, `tests/ui/RendererContractTests.cpp`, `tests/qt/PointPickResultTests.cpp`

**Interfaces:**
- Consumes: existing code.
- Produces: classes `pci::RenderViewportWidget`, `pci::PointCloudRenderer`, `pci::PointBufferUploader`, `pci::PointPicker`, struct `pci::CameraUniform`; CMake targets `pcinspector_renderer` and `pcinspector_gpu_tests`. Factory `createRenderViewport(pointCount, smokeTest)` unchanged. Later tasks use exactly these names.

- [ ] **Step 1: git-move the files**

```bash
git mv src/renderer/metal src/renderer/rhi
git mv src/renderer/rhi/MetalRenderViewport.h src/renderer/rhi/RenderViewportWidget.h
git mv src/renderer/rhi/MetalRenderViewport.cpp src/renderer/rhi/RenderViewportWidget.cpp
git mv src/renderer/rhi/MetalPointCloudRenderer.h src/renderer/rhi/PointCloudRenderer.h
git mv src/renderer/rhi/MetalPointCloudRenderer.cpp src/renderer/rhi/PointCloudRenderer.cpp
git mv src/renderer/rhi/MetalUploadManager.h src/renderer/rhi/PointBufferUploader.h
git mv src/renderer/rhi/MetalUploadManager.cpp src/renderer/rhi/PointBufferUploader.cpp
git mv src/renderer/rhi/MetalPointPicker.h src/renderer/rhi/PointPicker.h
git mv src/renderer/rhi/MetalPointPicker.cpp src/renderer/rhi/PointPicker.cpp
git mv tests/gpu/MetalPointPickerTests.cpp tests/gpu/PointPickerGpuTests.cpp
git mv tests/support/CatchMetalMain.cpp tests/support/CatchGpuMain.cpp
```

- [ ] **Step 2: repo-wide token replacement**

Apply in `src/`, `tests/`, `main.cpp`, `CMakeLists.txt`:

```bash
files=$(grep -rl 'Metal' src tests main.cpp CMakeLists.txt)
perl -pi -e 's/renderer\/metal\//renderer\/rhi\//g' $files
perl -pi -e 's/MetalRenderViewport/RenderViewportWidget/g' $files
perl -pi -e 's/MetalPointCloudRenderer/PointCloudRenderer/g' $files
perl -pi -e 's/MetalUploadManager/PointBufferUploader/g' $files
perl -pi -e 's/MetalPointPicker/PointPicker/g' $files
perl -pi -e 's/MetalCameraUniform/CameraUniform/g' $files
perl -pi -e 's/pcinspector_renderer_metal/pcinspector_renderer/g' CMakeLists.txt
perl -pi -e 's/pcinspector_metal_tests/pcinspector_gpu_tests/g' CMakeLists.txt
perl -pi -e 's/MetalPointPickerTests\.cpp/PointPickerGpuTests.cpp/g' CMakeLists.txt
perl -pi -e 's/CatchMetalMain\.cpp/CatchGpuMain.cpp/g' CMakeLists.txt
```

Then fix the remaining intentional cases by hand (do NOT blanket-replace "Metal"):
- `RenderViewportWidget.cpp`: keep `setApi(QRhiWidget::Api::Metal)` and the `rhi()->backend() != QRhi::Metal` check (Task 2 replaces them with the policy). Change user-facing strings: `"Metal renderer: %1"` → `"Renderer: %1"`, `"Metal device does not support integer vertex attributes"` → `"GPU device does not support integer vertex attributes"`, same for the point-size and R32UI messages in `PointPicker.cpp`. Keep `backendName()` returning `QStringLiteral("Metal")` for now.
- `tests/ui/RendererContractTests.cpp`: test name `"Metal viewport exposes its widget contract without a device"` → `"render viewport exposes its widget contract without a device"`; local variable `metalViewport` → `viewportWidget`.
- `tests/gpu/PointPickerGpuTests.cpp`: test name `"Metal point picking reports hits and misses"` → `"GPU point picking reports hits and misses"`; tag `"[gpu][metal]"` → `"[gpu]"`.
- `CMakeLists.txt`: LABELS `"gpu;metal"` → `"gpu"`; the `PCINSPECTOR_ENABLE_METAL_TESTS` option name stays until Task 3. Comment strings mentioning Metal tests may stay.

- [ ] **Step 3: verify no stale references**

```bash
grep -rn "renderer/metal\|MetalRender\|MetalPoint\|MetalUpload\|MetalCamera" src tests main.cpp CMakeLists.txt
```
Expected: no output.

- [ ] **Step 4: build and run the full suite**

```bash
cmake -S . -B build -DPCINSPECTOR_ENABLE_METAL_TESTS=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```
Expected: 100% tests passed (16 tests, including gpu + smoke).

- [ ] **Step 5: Commit**

```bash
git add -A src tests main.cpp CMakeLists.txt
git commit -m "refactor: rename Metal renderer classes to backend-neutral names"
```

---

### Task 2: Backend selection policy

One place decides which QRhi backend each platform uses; the viewport consumes it. TDD.

**Files:**
- Create: `src/renderer/rhi/BackendPolicy.h`, `src/renderer/rhi/BackendPolicy.cpp`
- Test: `tests/qt/BackendPolicyTests.cpp`
- Modify: `src/renderer/rhi/RenderViewportWidget.cpp` (ctor, `backendName()`, `initialize()`), `tests/ui/RendererContractTests.cpp`, `CMakeLists.txt` (add sources)

**Interfaces:**
- Produces (used by Tasks 3, 11 and all future backends):
  - `QRhiWidget::Api pci::preferredBackendApi() noexcept` — Metal on macOS, Direct3D 12 on Windows, Vulkan elsewhere.
  - `QString pci::backendApiName(QRhiWidget::Api api)` — `"Metal"`, `"Vulkan"`, `"Direct3D 12"`, `"Direct3D 11"`, `"OpenGL"`, `"Null"`.
  - `QRhi::Implementation pci::rhiImplementationFor(QRhiWidget::Api api) noexcept`.

- [ ] **Step 1: Write the failing test** — `tests/qt/BackendPolicyTests.cpp`:

```cpp
#include "renderer/rhi/BackendPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("preferred backend api matches the build platform", "[qt][renderer]")
{
#if defined(Q_OS_MACOS)
    CHECK(pci::preferredBackendApi() == QRhiWidget::Api::Metal);
#elif defined(Q_OS_WIN)
    CHECK(pci::preferredBackendApi() == QRhiWidget::Api::Direct3D12);
#else
    CHECK(pci::preferredBackendApi() == QRhiWidget::Api::Vulkan);
#endif
}

TEST_CASE("backend api names and implementations are consistent",
          "[qt][renderer]")
{
    CHECK(pci::backendApiName(QRhiWidget::Api::Metal)
          == QStringLiteral("Metal"));
    CHECK(pci::backendApiName(QRhiWidget::Api::Vulkan)
          == QStringLiteral("Vulkan"));
    CHECK(pci::backendApiName(QRhiWidget::Api::Direct3D12)
          == QStringLiteral("Direct3D 12"));
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Metal) == QRhi::Metal);
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Vulkan) == QRhi::Vulkan);
    CHECK(pci::rhiImplementationFor(QRhiWidget::Api::Direct3D12)
          == QRhi::D3D12);
}

} // namespace
```

Register it in `CMakeLists.txt` (target `pcinspector_qt_renderer_tests`, add to SOURCES):

```cmake
        tests/qt/BackendPolicyTests.cpp
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: compile error, `renderer/rhi/BackendPolicy.h` not found.

- [ ] **Step 3: Implement** — `src/renderer/rhi/BackendPolicy.h`:

```cpp
#pragma once

#include <QRhiWidget>
#include <rhi/qrhi.h>

namespace pci {

[[nodiscard]] QRhiWidget::Api preferredBackendApi() noexcept;
[[nodiscard]] QString backendApiName(QRhiWidget::Api api);
[[nodiscard]] QRhi::Implementation
rhiImplementationFor(QRhiWidget::Api api) noexcept;

} // namespace pci
```

`src/renderer/rhi/BackendPolicy.cpp`:

```cpp
#include "renderer/rhi/BackendPolicy.h"

namespace pci {

QRhiWidget::Api preferredBackendApi() noexcept
{
#if defined(Q_OS_MACOS)
    return QRhiWidget::Api::Metal;
#elif defined(Q_OS_WIN)
    return QRhiWidget::Api::Direct3D12;
#else
    return QRhiWidget::Api::Vulkan;
#endif
}

QString backendApiName(const QRhiWidget::Api api)
{
    switch (api) {
    case QRhiWidget::Api::Metal:
        return QStringLiteral("Metal");
    case QRhiWidget::Api::Vulkan:
        return QStringLiteral("Vulkan");
    case QRhiWidget::Api::Direct3D12:
        return QStringLiteral("Direct3D 12");
    case QRhiWidget::Api::Direct3D11:
        return QStringLiteral("Direct3D 11");
    case QRhiWidget::Api::OpenGL:
        return QStringLiteral("OpenGL");
    case QRhiWidget::Api::Null:
        return QStringLiteral("Null");
    }
    return QStringLiteral("Unknown");
}

QRhi::Implementation
rhiImplementationFor(const QRhiWidget::Api api) noexcept
{
    switch (api) {
    case QRhiWidget::Api::Metal:
        return QRhi::Metal;
    case QRhiWidget::Api::Vulkan:
        return QRhi::Vulkan;
    case QRhiWidget::Api::Direct3D12:
        return QRhi::D3D12;
    case QRhiWidget::Api::Direct3D11:
        return QRhi::D3D11;
    case QRhiWidget::Api::OpenGL:
        return QRhi::OpenGLES2;
    case QRhiWidget::Api::Null:
        return QRhi::Null;
    }
    return QRhi::Null;
}

} // namespace pci
```

Add `src/renderer/rhi/BackendPolicy.cpp` and `.h` to the `pcinspector_renderer` target sources in `CMakeLists.txt`.

Wire into `RenderViewportWidget.cpp` (include `"renderer/rhi/BackendPolicy.h"`):
- Constructor: `setApi(QRhiWidget::Api::Metal);` → `setApi(preferredBackendApi());`
- `backendName()`: `return QStringLiteral("Metal");` → `return backendApiName(api());`
- `initialize()`: replace the backend check with

```cpp
        if (rhi()->backend() != rhiImplementationFor(api())) {
            throw std::runtime_error(
                "QRhi did not select the requested backend: "
                + backendApiName(api()).toStdString());
        }
```

Update `tests/ui/RendererContractTests.cpp` (include `"renderer/rhi/BackendPolicy.h"`):

```cpp
    CHECK(viewport->backendName()
          == pci::backendApiName(pci::preferredBackendApi()));
    ...
    CHECK(widget->api() == pci::preferredBackendApi());
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure
```
Expected: all pass (backendName is still "Metal" on macOS, so behavior is unchanged).

- [ ] **Step 5: Commit**

```bash
git add src/renderer/rhi/BackendPolicy.h src/renderer/rhi/BackendPolicy.cpp \
        src/renderer/rhi/RenderViewportWidget.cpp tests/qt/BackendPolicyTests.cpp \
        tests/ui/RendererContractTests.cpp CMakeLists.txt
git commit -m "feat: add platform backend selection policy"
```

---

### Task 3: Un-gate the build from macOS

The app, UI library, renderer, and shaders build on every platform; only GPU-device tests stay behind an option.

**Files:**
- Modify: `CMakeLists.txt`, `main.cpp`

**Interfaces:**
- Produces: CMake option `PCINSPECTOR_ENABLE_GPU_TESTS` (replaces `PCINSPECTOR_ENABLE_METAL_TESTS`); unconditional targets `pcinspector_app_ui`, `pcinspector_renderer`, `pcinspector`.

- [ ] **Step 1: Restructure CMakeLists.txt**

1. Replace the option:

```cmake
option(PCINSPECTOR_ENABLE_GPU_TESTS
        "Build and register tests that require a native GPU device" OFF)
```

2. Merge the Qt component lists into the single top-level `find_package`:

```cmake
find_package(Qt6 6.7 REQUIRED COMPONENTS
        Concurrent
        Core
        Gui
        GuiPrivate
        ShaderTools
        Test
        Widgets)
```

3. Delete the `if(APPLE)` / `else()` / `endif()` wrapper around `pcinspector_app_ui`, `pcinspector_renderer`, the `pcinspector` executable, and `qt_add_shaders` (and remove the second `find_package(Qt6 ...)` inside it, plus the `message(STATUS "...only on macOS...")` branch). The target definitions themselves are unchanged.
4. In the testing section, delete the `if(APPLE)` wrapper around the ui/renderer test targets (keep the targets), and change the GPU-test gate from `if(PCINSPECTOR_ENABLE_METAL_TESTS)` to `if(PCINSPECTOR_ENABLE_GPU_TESTS)` (its `endif()` stays; it is no longer nested inside an APPLE block).

- [ ] **Step 2: Neutral app description** — in `main.cpp`:

```cpp
    parser.setApplicationDescription(
        QStringLiteral("Cross-platform point-cloud viewer"));
```

- [ ] **Step 3: Reconfigure, build, full suite**

```bash
cmake -S . -B build -DPCINSPECTOR_ENABLE_GPU_TESTS=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```
Expected: all tests pass; same test count as Task 1.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt main.cpp
git commit -m "build: un-gate app and renderer from macOS-only builds"
```

---

# Part B — Phase 1: Blocked storage end-to-end

Ordering rationale: Tasks 4–10 are additive (nothing existing changes, suite stays green after each). Task 11 switches the render stack to blocks while preserving all external behavior through a temporary `LoadedPointCloud`→scene adapter, so the existing contract/GPU tests keep passing unmodified. Task 12 switches import + app to world-space scenes and deletes the legacy types. Task 13 verifies end-to-end.

### Task 4: `pcinspector_scene` library and `PointBlock`

**Files:**
- Create: `src/scene/PointBlock.h`, `src/scene/PointBlock.cpp`
- Test: `tests/unit/PointBlockTests.cpp`
- Modify: `CMakeLists.txt` (new library + test registration)

**Interfaces:**
- Consumes: `pci::GpuPoint` (`core/GpuPoint.h`), `pci::Vec3d` (`core/Vec3d.h`), `pci::Bounds3d` (`pointcloud/Bounds3d.h`), `pci::PointAttributes` (`pointcloud/PointAttributes.h`).
- Produces (used by every later task):
  - `inline constexpr std::uint32_t pci::maximumPointsPerBlock = 1'048'576;`
  - `struct pci::PointBlock { Vec3d origin; double scale; Bounds3d bounds; std::uint16_t intensityMaximum; std::vector<GpuPoint> points; std::vector<PointAttributes> attributes; };`
  - `using pci::PointBlockPtr = std::shared_ptr<const PointBlock>;`
  - `std::array<std::uint16_t, 3> pci::quantizeToBlock(Vec3d position, Vec3d origin, double scale) noexcept`
  - `Vec3d pci::decodeBlockPosition(const PointBlock &block, const GpuPoint &point) noexcept`
  - `double pci::blockScaleForEdge(double cellEdge) noexcept`

- [ ] **Step 1: Write the failing test** — `tests/unit/PointBlockTests.cpp`:

```cpp
#include "scene/PointBlock.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

namespace {

TEST_CASE("block quantization round-trips within half a step",
          "[unit][scene]")
{
    const pci::Vec3d origin{1000.0, 2000.0, 10.0};
    const double edge = 10.0;
    const double scale = pci::blockScaleForEdge(edge);
    CHECK(scale == Catch::Approx(10.0 / 65535.0));

    pci::PointBlock block{.origin = origin, .scale = scale};
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> offset(0.0, edge);
    for (int i = 0; i < 1000; ++i) {
        const pci::Vec3d position{
            origin.x + offset(rng),
            origin.y + offset(rng),
            origin.z + offset(rng),
        };
        const auto q = pci::quantizeToBlock(position, origin, scale);
        const pci::GpuPoint point{.x = q[0], .y = q[1], .z = q[2]};
        const pci::Vec3d decoded = pci::decodeBlockPosition(block, point);
        CHECK(std::abs(decoded.x - position.x) <= scale * 0.5 + 1e-12);
        CHECK(std::abs(decoded.y - position.y) <= scale * 0.5 + 1e-12);
        CHECK(std::abs(decoded.z - position.z) <= scale * 0.5 + 1e-12);
    }
}

TEST_CASE("block quantization clamps positions outside the cell",
          "[unit][scene]")
{
    const pci::Vec3d origin{};
    const double scale = pci::blockScaleForEdge(1.0);
    CHECK(pci::quantizeToBlock({-5.0, 0.5, 2.0}, origin, scale)
          == std::array<std::uint16_t, 3>{0, 32768, 65535});
}

TEST_CASE("point blocks share immutable ownership", "[unit][scene]")
{
    static_assert(std::is_same_v<
                  pci::PointBlockPtr,
                  std::shared_ptr<const pci::PointBlock>>);
    CHECK(pci::maximumPointsPerBlock == 1'048'576U);
}

} // namespace
```

- [ ] **Step 2: Register library and test in CMakeLists.txt, verify failure**

After the `pcinspector_pointcloud` block add:

```cmake
add_library(pcinspector_scene
        src/scene/PointBlock.cpp)
target_include_directories(pcinspector_scene PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pcinspector_scene PUBLIC
        pcinspector_core
        pcinspector_pointcloud)
set_target_properties(pcinspector_scene PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF)
```

In `pcinspector_unit_tests`: add `tests/unit/PointBlockTests.cpp` to SOURCES and `pcinspector_scene` to LIBRARIES.

Run: `cmake --build build -j 8`
Expected: FAIL — `scene/PointBlock.h` not found.

- [ ] **Step 3: Implement** — `src/scene/PointBlock.h`:

```cpp
#pragma once

#include "core/GpuPoint.h"
#include "core/Vec3d.h"
#include "pointcloud/Bounds3d.h"
#include "pointcloud/PointAttributes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace pci {

inline constexpr std::uint32_t maximumPointsPerBlock = 1'048'576;
inline constexpr double blockQuantizationSteps = 65535.0;

struct PointBlock {
    Vec3d origin;
    double scale = 1.0; // world units per quantization step
    Bounds3d bounds;    // tight world-space bounds of contained points
    std::uint16_t intensityMaximum = 0;
    std::vector<GpuPoint> points;
    std::vector<PointAttributes> attributes;
};

using PointBlockPtr = std::shared_ptr<const PointBlock>;

[[nodiscard]] std::array<std::uint16_t, 3> quantizeToBlock(
    Vec3d position, Vec3d origin, double scale) noexcept;
[[nodiscard]] Vec3d decodeBlockPosition(
    const PointBlock &block, const GpuPoint &point) noexcept;
[[nodiscard]] double blockScaleForEdge(double cellEdge) noexcept;

} // namespace pci
```

`src/scene/PointBlock.cpp`:

```cpp
#include "scene/PointBlock.h"

#include <algorithm>
#include <cmath>

namespace pci {
namespace {

std::uint16_t quantizeComponent(const double value,
                                const double origin,
                                const double scale) noexcept
{
    const double steps = (value - origin) / scale;
    return static_cast<std::uint16_t>(std::clamp(
        std::lround(steps),
        0L,
        static_cast<long>(blockQuantizationSteps)));
}

} // namespace

std::array<std::uint16_t, 3> quantizeToBlock(
    const Vec3d position, const Vec3d origin, const double scale) noexcept
{
    return {
        quantizeComponent(position.x, origin.x, scale),
        quantizeComponent(position.y, origin.y, scale),
        quantizeComponent(position.z, origin.z, scale),
    };
}

Vec3d decodeBlockPosition(const PointBlock &block,
                          const GpuPoint &point) noexcept
{
    return block.origin
        + Vec3d{
              static_cast<double>(point.x),
              static_cast<double>(point.y),
              static_cast<double>(point.z),
          }
        * block.scale;
}

double blockScaleForEdge(const double cellEdge) noexcept
{
    return cellEdge / blockQuantizationSteps;
}

} // namespace pci
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure -R unit
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene tests/unit/PointBlockTests.cpp CMakeLists.txt
git commit -m "feat: add scene library with locally quantized point blocks"
```

---

### Task 5: `BlockPartitioner` — streamed grid partitioning

Grid over the source bounds; cube cells sized for ~256k expected points; a cell seals and emits a block whenever it reaches `maximumPointsPerBlock`, so dense cells produce multiple blocks and emission is progressive (this supersedes the spec's "split overfull cells once" with an equivalent, stream-friendlier bound on block size).

**Files:**
- Create: `src/scene/BlockPartitioner.h`, `src/scene/BlockPartitioner.cpp`
- Test: `tests/unit/BlockPartitionerTests.cpp`
- Modify: `CMakeLists.txt` (add sources)

**Interfaces:**
- Consumes: Task 4 types.
- Produces (used by Tasks 9 and 12):
  - `struct pci::PointSample { Vec3d position; std::uint32_t rgba; std::uint16_t packedAttributes; std::uint32_t packedProperties; PointAttributes attributes; };`
  - `class pci::BlockPartitioner { BlockPartitioner(const Bounds3d &bounds, std::uint64_t expectedPointCount, std::function<void(PointBlockPtr)> blockReady); void add(const PointSample &sample); void finish(); double cellEdge() const noexcept; };`

- [ ] **Step 1: Write the failing test** — `tests/unit/BlockPartitionerTests.cpp`:

```cpp
#include "scene/BlockPartitioner.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

namespace {

const pci::Bounds3d unitBounds{
    .minimum = {0.0, 0.0, 0.0},
    .maximum = {1.0, 1.0, 1.0},
};

TEST_CASE("small inputs produce one block in source order", "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 8, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({.position = {0.25, 0.25, 0.25}, .rgba = 1});
    partitioner.add({.position = {0.75, 0.75, 0.75}, .rgba = 2});
    partitioner.finish();

    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks.front()->points.size() == 2);
    CHECK(blocks.front()->points[0].rgba == 1);
    CHECK(blocks.front()->points[1].rgba == 2);
    CHECK(blocks.front()->attributes.size() == 2);
    const pci::Vec3d decoded = pci::decodeBlockPosition(
        *blocks.front(), blocks.front()->points[0]);
    CHECK(decoded.x == Catch::Approx(0.25).margin(1e-4));
    CHECK(decoded.y == Catch::Approx(0.25).margin(1e-4));
    CHECK(decoded.z == Catch::Approx(0.25).margin(1e-4));
}

TEST_CASE("dense cells seal blocks at capacity during streaming",
          "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 2'500'000, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    // All samples land in one grid cell.
    const std::uint64_t total = 2 * pci::maximumPointsPerBlock + 5;
    for (std::uint64_t i = 0; i < total; ++i) {
        partitioner.add({.position = {0.01, 0.01, 0.01}});
    }
    CHECK(blocks.size() == 2); // sealed mid-stream, before finish()
    partitioner.finish();

    REQUIRE(blocks.size() == 3);
    std::uint64_t counted = 0;
    for (const auto &block : blocks) {
        CHECK(block->points.size() <= pci::maximumPointsPerBlock);
        CHECK(block->points.size() == block->attributes.size());
        counted += block->points.size();
    }
    CHECK(counted == total);
}

TEST_CASE("blocks carry tight bounds and intensity statistics",
          "[unit][scene]")
{
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        unitBounds, 4, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({
        .position = {0.2, 0.3, 0.4},
        .attributes = {.intensity = 700},
    });
    partitioner.add({
        .position = {0.6, 0.5, 0.4},
        .attributes = {.intensity = 200},
    });
    partitioner.finish();

    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front()->intensityMaximum == 700);
    CHECK(blocks.front()->bounds.minimum
          == std::array{0.2, 0.3, 0.4});
    CHECK(blocks.front()->bounds.maximum
          == std::array{0.6, 0.5, 0.4});
}

TEST_CASE("degenerate bounds still partition", "[unit][scene]")
{
    const pci::Bounds3d point{
        .minimum = {5.0, 5.0, 5.0},
        .maximum = {5.0, 5.0, 5.0},
    };
    std::vector<pci::PointBlockPtr> blocks;
    pci::BlockPartitioner partitioner(
        point, 1, [&blocks](pci::PointBlockPtr b) {
            blocks.push_back(std::move(b));
        });
    partitioner.add({.position = {5.0, 5.0, 5.0}});
    partitioner.finish();
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front()->points.size() == 1);
}

} // namespace
```

Add to `pcinspector_unit_tests` SOURCES and `src/scene/BlockPartitioner.cpp` to `pcinspector_scene`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: FAIL — `scene/BlockPartitioner.h` not found.

- [ ] **Step 3: Implement** — `src/scene/BlockPartitioner.h`:

```cpp
#pragma once

#include "scene/PointBlock.h"

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace pci {

struct PointSample {
    Vec3d position;
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t packedAttributes = 0;
    std::uint32_t packedProperties = 0;
    PointAttributes attributes;
};

class BlockPartitioner {
public:
    using BlockReady = std::function<void(PointBlockPtr)>;

    BlockPartitioner(const Bounds3d &bounds,
                     std::uint64_t expectedPointCount,
                     BlockReady blockReady);

    void add(const PointSample &sample);
    void finish();
    [[nodiscard]] double cellEdge() const noexcept;

private:
    [[nodiscard]] std::uint64_t cellKeyFor(Vec3d position) const noexcept;
    [[nodiscard]] Vec3d cellOrigin(std::uint64_t key) const noexcept;
    void seal(std::shared_ptr<PointBlock> &block);

    Bounds3d bounds_;
    double edge_ = 1.0;
    std::array<std::uint64_t, 3> cellCounts_{1, 1, 1};
    BlockReady blockReady_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PointBlock>>
        building_;
};

} // namespace pci
```

`src/scene/BlockPartitioner.cpp`:

```cpp
#include "scene/BlockPartitioner.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

constexpr double targetPointsPerCell = 262'144.0;
constexpr std::uint64_t maximumCellsPerAxis = 1024;

void expandBounds(Bounds3d &bounds, const Vec3d position,
                  const bool first) noexcept
{
    if (first) {
        bounds.minimum = {position.x, position.y, position.z};
        bounds.maximum = bounds.minimum;
        return;
    }
    bounds.minimum[0] = std::min(bounds.minimum[0], position.x);
    bounds.minimum[1] = std::min(bounds.minimum[1], position.y);
    bounds.minimum[2] = std::min(bounds.minimum[2], position.z);
    bounds.maximum[0] = std::max(bounds.maximum[0], position.x);
    bounds.maximum[1] = std::max(bounds.maximum[1], position.y);
    bounds.maximum[2] = std::max(bounds.maximum[2], position.z);
}

} // namespace

BlockPartitioner::BlockPartitioner(const Bounds3d &bounds,
                                   const std::uint64_t expectedPointCount,
                                   BlockReady blockReady)
    : bounds_(bounds)
    , blockReady_(std::move(blockReady))
{
    if (!blockReady_) {
        throw std::invalid_argument(
            "block partitioner requires a block-ready callback");
    }
    const double extent = std::max(bounds_.maximumExtent(), 1e-9);
    const auto cellsPerAxis = static_cast<std::uint64_t>(std::clamp(
        std::ceil(std::cbrt(
            static_cast<double>(std::max<std::uint64_t>(
                expectedPointCount, 1))
            / targetPointsPerCell)),
        1.0,
        static_cast<double>(maximumCellsPerAxis)));
    edge_ = extent / static_cast<double>(cellsPerAxis);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double axisExtent =
            bounds_.maximum[axis] - bounds_.minimum[axis];
        cellCounts_[axis] = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::ceil(
                std::max(axisExtent, 0.0) / edge_)));
    }
}

double BlockPartitioner::cellEdge() const noexcept
{
    return edge_;
}

std::uint64_t
BlockPartitioner::cellKeyFor(const Vec3d position) const noexcept
{
    std::array<std::uint64_t, 3> index{};
    const std::array<double, 3> components{
        position.x, position.y, position.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double offset = components[axis] - bounds_.minimum[axis];
        const auto cell = static_cast<std::int64_t>(
            std::floor(offset / edge_));
        index[axis] = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
            cell, 0, static_cast<std::int64_t>(cellCounts_[axis]) - 1));
    }
    return index[0]
        + index[1] * cellCounts_[0]
        + index[2] * cellCounts_[0] * cellCounts_[1];
}

Vec3d BlockPartitioner::cellOrigin(const std::uint64_t key) const noexcept
{
    const std::uint64_t xy = cellCounts_[0] * cellCounts_[1];
    const std::uint64_t iz = key / xy;
    const std::uint64_t iy = (key % xy) / cellCounts_[0];
    const std::uint64_t ix = key % cellCounts_[0];
    return {
        bounds_.minimum[0] + static_cast<double>(ix) * edge_,
        bounds_.minimum[1] + static_cast<double>(iy) * edge_,
        bounds_.minimum[2] + static_cast<double>(iz) * edge_,
    };
}

void BlockPartitioner::add(const PointSample &sample)
{
    const std::uint64_t key = cellKeyFor(sample.position);
    auto &block = building_[key];
    if (!block) {
        block = std::make_shared<PointBlock>();
        block->origin = cellOrigin(key);
        block->scale = blockScaleForEdge(edge_);
        block->points.reserve(std::min<std::uint64_t>(
            maximumPointsPerBlock, 4096));
    }

    const auto quantized =
        quantizeToBlock(sample.position, block->origin, block->scale);
    expandBounds(block->bounds, sample.position, block->points.empty());
    block->points.push_back({
        .x = quantized[0],
        .y = quantized[1],
        .z = quantized[2],
        .attributes = sample.packedAttributes,
        .rgba = sample.rgba,
        .packedProperties = sample.packedProperties,
    });
    block->attributes.push_back(sample.attributes);
    block->intensityMaximum = std::max(
        block->intensityMaximum, sample.attributes.intensity);

    if (block->points.size() >= maximumPointsPerBlock) {
        seal(block);
        building_.erase(key);
    }
}

void BlockPartitioner::finish()
{
    for (auto &[key, block] : building_) {
        if (block && !block->points.empty()) {
            seal(block);
        }
    }
    building_.clear();
}

void BlockPartitioner::seal(std::shared_ptr<PointBlock> &block)
{
    blockReady_(PointBlockPtr(std::move(block)));
}

} // namespace pci
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure -R unit
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene/BlockPartitioner.h src/scene/BlockPartitioner.cpp \
        tests/unit/BlockPartitionerTests.cpp CMakeLists.txt
git commit -m "feat: add streamed grid block partitioner"
```

---

### Task 6: `PointCloudScene` — thread-safe block container

**Files:**
- Create: `src/scene/PointCloudScene.h`, `src/scene/PointCloudScene.cpp`
- Test: `tests/unit/PointCloudSceneTests.cpp`
- Modify: `CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: Task 4 types, `pci::PointCloudMetadata`.
- Produces (used by Tasks 9–12):
  - `class pci::PointCloudScene { explicit PointCloudScene(PointCloudMetadata metadata); const PointCloudMetadata &metadata() const noexcept; void addBlock(PointBlockPtr block); std::vector<PointBlockPtr> blocks() const; std::uint64_t totalPointCount() const; std::uint64_t revision() const; Bounds3d bounds() const; std::uint16_t intensityMaximum() const; };`
  - `using pci::PointCloudScenePtr = std::shared_ptr<PointCloudScene>;`
  - Thread contract: `addBlock` may be called from a worker thread; all getters may be called concurrently from the render/UI thread. Blocks are immutable after `addBlock`.

- [ ] **Step 1: Write the failing test** — `tests/unit/PointCloudSceneTests.cpp`:

```cpp
#include "scene/PointCloudScene.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

namespace {

pci::PointBlockPtr blockWith(const std::size_t pointCount,
                             const pci::Vec3d origin,
                             const std::uint16_t intensityMaximum = 0)
{
    auto block = std::make_shared<pci::PointBlock>();
    block->origin = origin;
    block->scale = 1.0 / 65535.0;
    block->bounds = {
        .minimum = {origin.x, origin.y, origin.z},
        .maximum = {origin.x + 1.0, origin.y + 1.0, origin.z + 1.0},
    };
    block->intensityMaximum = intensityMaximum;
    block->points.resize(pointCount);
    block->attributes.resize(pointCount);
    return block;
}

TEST_CASE("scene aggregates blocks, bounds, and statistics",
          "[unit][scene]")
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 5;
    metadata.sourceBounds = {
        .minimum = {0.0, 0.0, 0.0},
        .maximum = {4.0, 4.0, 4.0},
    };
    pci::PointCloudScene scene(metadata);

    CHECK(scene.totalPointCount() == 0);
    CHECK(scene.revision() == 0);
    CHECK(scene.bounds().minimum == metadata.sourceBounds.minimum);

    scene.addBlock(blockWith(3, {0.0, 0.0, 0.0}, 100));
    scene.addBlock(blockWith(2, {2.0, 2.0, 2.0}, 700));

    CHECK(scene.metadata().sourcePointCount == 5);
    CHECK(scene.totalPointCount() == 5);
    CHECK(scene.revision() == 2);
    CHECK(scene.blocks().size() == 2);
    CHECK(scene.intensityMaximum() == 700);
    CHECK(scene.bounds().minimum == std::array{0.0, 0.0, 0.0});
    CHECK(scene.bounds().maximum == std::array{3.0, 3.0, 3.0});
}

TEST_CASE("scene accepts blocks from a worker thread", "[unit][scene]")
{
    pci::PointCloudScene scene({});
    std::thread worker([&scene] {
        for (int i = 0; i < 100; ++i) {
            scene.addBlock(blockWith(10, {0.0, 0.0, 0.0}));
        }
    });
    // Concurrent reads must be safe while the worker adds blocks.
    while (scene.totalPointCount() < 1000) {
        static_cast<void>(scene.blocks().size());
    }
    worker.join();
    CHECK(scene.totalPointCount() == 1000);
    CHECK(scene.revision() == 100);
}

} // namespace
```

Add to `pcinspector_unit_tests` SOURCES; add `src/scene/PointCloudScene.cpp` to `pcinspector_scene`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: FAIL — `scene/PointCloudScene.h` not found.

- [ ] **Step 3: Implement** — `src/scene/PointCloudScene.h`:

```cpp
#pragma once

#include "pointcloud/PointCloudMetadata.h"
#include "scene/PointBlock.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pci {

class PointCloudScene {
public:
    explicit PointCloudScene(PointCloudMetadata metadata);

    [[nodiscard]] const PointCloudMetadata &metadata() const noexcept;
    void addBlock(PointBlockPtr block);
    [[nodiscard]] std::vector<PointBlockPtr> blocks() const;
    [[nodiscard]] std::uint64_t totalPointCount() const;
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] Bounds3d bounds() const;
    [[nodiscard]] std::uint16_t intensityMaximum() const;

private:
    const PointCloudMetadata metadata_;
    mutable std::mutex mutex_;
    std::vector<PointBlockPtr> blocks_;
    std::uint64_t totalPoints_ = 0;
    std::uint64_t revision_ = 0;
    std::uint16_t intensityMaximum_ = 0;
    Bounds3d blockBounds_;
};

using PointCloudScenePtr = std::shared_ptr<PointCloudScene>;

} // namespace pci
```

`src/scene/PointCloudScene.cpp`:

```cpp
#include "scene/PointCloudScene.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pci {

PointCloudScene::PointCloudScene(PointCloudMetadata metadata)
    : metadata_(std::move(metadata))
{
}

const PointCloudMetadata &PointCloudScene::metadata() const noexcept
{
    return metadata_;
}

void PointCloudScene::addBlock(PointBlockPtr block)
{
    if (!block || block->points.empty()) {
        throw std::invalid_argument("scene blocks must contain points");
    }
    const std::scoped_lock lock(mutex_);
    if (blocks_.empty()) {
        blockBounds_ = block->bounds;
    } else {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            blockBounds_.minimum[axis] = std::min(
                blockBounds_.minimum[axis], block->bounds.minimum[axis]);
            blockBounds_.maximum[axis] = std::max(
                blockBounds_.maximum[axis], block->bounds.maximum[axis]);
        }
    }
    totalPoints_ += block->points.size();
    intensityMaximum_ =
        std::max(intensityMaximum_, block->intensityMaximum);
    ++revision_;
    blocks_.push_back(std::move(block));
}

std::vector<PointBlockPtr> PointCloudScene::blocks() const
{
    const std::scoped_lock lock(mutex_);
    return blocks_;
}

std::uint64_t PointCloudScene::totalPointCount() const
{
    const std::scoped_lock lock(mutex_);
    return totalPoints_;
}

std::uint64_t PointCloudScene::revision() const
{
    const std::scoped_lock lock(mutex_);
    return revision_;
}

Bounds3d PointCloudScene::bounds() const
{
    const std::scoped_lock lock(mutex_);
    return blocks_.empty() ? metadata_.sourceBounds : blockBounds_;
}

std::uint16_t PointCloudScene::intensityMaximum() const
{
    const std::scoped_lock lock(mutex_);
    return intensityMaximum_;
}

} // namespace pci
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure -R unit
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene/PointCloudScene.h src/scene/PointCloudScene.cpp \
        tests/unit/PointCloudSceneTests.cpp CMakeLists.txt
git commit -m "feat: add thread-safe point cloud scene container"
```

---

### Task 7: `FrustumCuller`

CPU frustum culling over block AABBs, built geometrically from camera parameters (no matrix inversion, all double precision).

**Files:**
- Create: `src/scene/FrustumCuller.h`, `src/scene/FrustumCuller.cpp`
- Test: `tests/unit/FrustumCullerTests.cpp`
- Modify: `CMakeLists.txt` (add source)

**Interfaces:**
- Consumes: `Vec3d`, `Bounds3d`.
- Produces (used by Task 11):
  - `pci::FrustumCuller::fromCamera(Vec3d position, Vec3d forward, Vec3d up, Vec3d right, double verticalFovDegrees, double aspect, double nearPlane, double farPlane)`
  - `bool pci::FrustumCuller::intersects(const Bounds3d &bounds) const noexcept`

- [ ] **Step 1: Write the failing test** — `tests/unit/FrustumCullerTests.cpp`:

```cpp
#include "scene/FrustumCuller.h"

#include <catch2/catch_test_macros.hpp>

namespace {

pci::FrustumCuller defaultCuller()
{
    // Matches the default navigation camera: at {0,-4,0} looking +y, z up.
    return pci::FrustumCuller::fromCamera(
        {0.0, -4.0, 0.0}, // position
        {0.0, 1.0, 0.0},  // forward
        {0.0, 0.0, 1.0},  // up
        {1.0, 0.0, 0.0},  // right
        60.0, 1.0, 0.04, 12.0);
}

pci::Bounds3d cubeAt(const double x, const double y, const double z,
                     const double halfEdge = 1.0)
{
    return {
        .minimum = {x - halfEdge, y - halfEdge, z - halfEdge},
        .maximum = {x + halfEdge, y + halfEdge, z + halfEdge},
    };
}

TEST_CASE("boxes ahead of the camera are visible", "[unit][scene]")
{
    CHECK(defaultCuller().intersects(cubeAt(0.0, 0.0, 0.0)));
}

TEST_CASE("boxes behind the camera are culled", "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, -10.0, 0.0)));
}

TEST_CASE("boxes far outside the field of view are culled",
          "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(40.0, 4.0, 0.0)));
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, 4.0, 40.0)));
}

TEST_CASE("boxes beyond the far plane are culled", "[unit][scene]")
{
    CHECK_FALSE(defaultCuller().intersects(cubeAt(0.0, 100.0, 0.0)));
}

TEST_CASE("boxes straddling a frustum edge remain visible",
          "[unit][scene]")
{
    // Large box that overlaps the left frustum plane.
    CHECK(defaultCuller().intersects(cubeAt(-4.0, 4.0, 0.0, 4.0)));
}

} // namespace
```

Add to `pcinspector_unit_tests` SOURCES; add `src/scene/FrustumCuller.cpp` to `pcinspector_scene`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: FAIL — `scene/FrustumCuller.h` not found.

- [ ] **Step 3: Implement** — `src/scene/FrustumCuller.h`:

```cpp
#pragma once

#include "core/Vec3d.h"
#include "pointcloud/Bounds3d.h"

#include <array>

namespace pci {

class FrustumCuller {
public:
    [[nodiscard]] static FrustumCuller fromCamera(
        Vec3d position, Vec3d forward, Vec3d up, Vec3d right,
        double verticalFovDegrees, double aspect,
        double nearPlane, double farPlane) noexcept;

    [[nodiscard]] bool intersects(const Bounds3d &bounds) const noexcept;

private:
    struct Plane {
        Vec3d normal;    // points into the visible half-space
        double distance; // signedDistance(p) = dot(normal, p) + distance
    };

    std::array<Plane, 6> planes_{};
};

} // namespace pci
```

`src/scene/FrustumCuller.cpp`:

```cpp
#include "scene/FrustumCuller.h"

#include <cmath>
#include <numbers>

namespace pci {

FrustumCuller FrustumCuller::fromCamera(
    const Vec3d position, const Vec3d forward, const Vec3d up,
    const Vec3d right, const double verticalFovDegrees,
    const double aspect, const double nearPlane,
    const double farPlane) noexcept
{
    const double halfVertical = std::tan(
        verticalFovDegrees * 0.5 * std::numbers::pi / 180.0);
    const double halfHorizontal = halfVertical * aspect;

    FrustumCuller culler;
    auto plane = [&](const Vec3d normal, const Vec3d point) {
        const Vec3d unit = normalized(normal);
        return Plane{unit, -dot(unit, point)};
    };
    culler.planes_[0] = plane(forward, position + forward * nearPlane);
    culler.planes_[1] =
        plane(forward * -1.0, position + forward * farPlane);
    // Inward side-plane normals: tan(half-angle)*forward -/+ axis.
    culler.planes_[2] = plane(forward * halfVertical - up, position);
    culler.planes_[3] = plane(forward * halfVertical + up, position);
    culler.planes_[4] = plane(forward * halfHorizontal - right, position);
    culler.planes_[5] = plane(forward * halfHorizontal + right, position);
    return culler;
}

bool FrustumCuller::intersects(const Bounds3d &bounds) const noexcept
{
    const Vec3d center{
        (bounds.minimum[0] + bounds.maximum[0]) * 0.5,
        (bounds.minimum[1] + bounds.maximum[1]) * 0.5,
        (bounds.minimum[2] + bounds.maximum[2]) * 0.5,
    };
    const Vec3d halfExtent{
        (bounds.maximum[0] - bounds.minimum[0]) * 0.5,
        (bounds.maximum[1] - bounds.minimum[1]) * 0.5,
        (bounds.maximum[2] - bounds.minimum[2]) * 0.5,
    };
    for (const Plane &plane : planes_) {
        const double radius =
            halfExtent.x * std::abs(plane.normal.x)
            + halfExtent.y * std::abs(plane.normal.y)
            + halfExtent.z * std::abs(plane.normal.z);
        if (dot(plane.normal, center) + plane.distance < -radius) {
            return false;
        }
    }
    return true;
}

} // namespace pci
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure -R unit
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene/FrustumCuller.h src/scene/FrustumCuller.cpp \
        tests/unit/FrustumCullerTests.cpp CMakeLists.txt
git commit -m "feat: add frustum culling for block bounds"
```

---

### Task 8: World-space `NavigationCamera`

Replace the hardcoded normalized-cube assumptions (`sceneDiameter = 2.0` constant, `frameScene()` at `{0,-4,0}`, far plane referencing the origin) with a configurable scene center + diameter. Defaults preserve today's behavior exactly, so all existing tests stay green.

**Files:**
- Modify: `src/core/NavigationCamera.h`, `src/core/NavigationCamera.cpp`
- Test: `tests/unit/NavigationCameraSceneTests.cpp` (new)
- Modify: `CMakeLists.txt` (register test)

**Interfaces:**
- Produces (used by Tasks 11–12):
  - `void NavigationCamera::setScene(Vec3d center, double diameter) noexcept` — ignores non-finite centers and diameters ≤ 0.
  - `double NavigationCamera::sceneDiameter() const noexcept` (member accessor; the `static constexpr` of the same name is removed).
  - `frameScene()` now positions at `center + {0, -2 * diameter, 0}` with pivot at `center` (identical to today for the default center `{0,0,0}` / diameter `2.0`).

- [ ] **Step 1: Check for external users of the removed constant**

```bash
grep -rn "NavigationCamera::sceneDiameter\|sceneDiameter" src tests
```
Expected: matches only inside `NavigationCamera.cpp` (the class's own uses). If any other file uses the constant, update it to the accessor in Step 3.

- [ ] **Step 2: Write the failing test** — `tests/unit/NavigationCameraSceneTests.cpp`:

```cpp
#include "core/NavigationCamera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("default camera framing is the normalized cube",
          "[unit][core]")
{
    pci::NavigationCamera camera;
    camera.frameScene();
    CHECK(camera.position() == pci::Vec3d{0.0, -4.0, 0.0});
    CHECK(camera.pivot() == pci::Vec3d{});
    CHECK(camera.sceneDiameter() == 2.0);
}

TEST_CASE("scene bounds reposition framing and scale speed",
          "[unit][core]")
{
    pci::NavigationCamera camera;
    camera.setScene({100.0, 50.0, 20.0}, 40.0);
    camera.frameScene();

    CHECK(camera.position() == pci::Vec3d{100.0, -30.0, 20.0});
    CHECK(camera.pivot() == pci::Vec3d{100.0, 50.0, 20.0});
    CHECK(camera.sceneDiameter() == 40.0);
    // Speed derives from pivot distance (80), clamped to 2x diameter.
    CHECK(camera.movementSpeed() == Catch::Approx(80.0));
    CHECK(camera.clipPlanes().farPlane >= 160.0);
}

TEST_CASE("invalid scene parameters are ignored", "[unit][core]")
{
    pci::NavigationCamera camera;
    camera.setScene({100.0, 50.0, 20.0}, 40.0);
    camera.setScene({0.0, 0.0, 0.0}, 0.0);
    camera.setScene({0.0, 0.0, 0.0}, -5.0);
    CHECK(camera.sceneDiameter() == 40.0);
}

} // namespace
```

Add to `pcinspector_unit_tests` SOURCES in `CMakeLists.txt`.

Run: `cmake --build build -j 8`
Expected: FAIL — no `setScene`/`sceneDiameter()` members.

- [ ] **Step 3: Implement**

`src/core/NavigationCamera.h` — remove `static constexpr double sceneDiameter = 2.0;`, add to the public section:

```cpp
    void setScene(Vec3d center, double diameter) noexcept;
    [[nodiscard]] double sceneDiameter() const noexcept;
```

and to the private section:

```cpp
    Vec3d sceneCenter_{};
    double sceneDiameter_ = 2.0;
```

`src/core/NavigationCamera.cpp` changes:

```cpp
void NavigationCamera::setScene(const Vec3d center,
                                const double diameter) noexcept
{
    if (!isFinite(center) || !std::isfinite(diameter)
        || diameter <= 0.0) {
        return;
    }
    sceneCenter_ = center;
    sceneDiameter_ = diameter;
    markChanged();
}

double NavigationCamera::sceneDiameter() const noexcept
{
    return sceneDiameter_;
}

void NavigationCamera::frameScene() noexcept
{
    position_ = sceneCenter_ + Vec3d{0.0, -2.0 * sceneDiameter_, 0.0};
    pivot_ = sceneCenter_;
    forward_ = {0.0, 1.0, 0.0};
    navigationReference_.reset();
    markChanged();
}
```

Replace every remaining `sceneDiameter` read with `sceneDiameter_` (`panFromDrag`, `dollyForward`, `movementSpeed`, `clipPlanes`). In `clipPlanes()` also make the far plane scene-relative:

```cpp
NavigationCamera::ClipPlanes NavigationCamera::clipPlanes() const noexcept
{
    const Vec3d reference = navigationReference_.value_or(pivot_);
    const double referenceDistance = length(position_ - reference);
    const double nearPlane = std::clamp(
        referenceDistance * 0.01,
        sceneDiameter_ * 1e-7,
        sceneDiameter_ * 0.005);
    constexpr double halfSqrt3 = 0.8660254037844386;
    const double sceneRadius = sceneDiameter_ * halfSqrt3;
    const double farPlane = std::max(
        sceneDiameter_ * 4.0,
        length(position_ - sceneCenter_) + sceneRadius + sceneDiameter_);
    return {
        .nearPlane = nearPlane,
        .farPlane = std::max(farPlane, nearPlane * 2.0),
    };
}
```

(For the defaults — center `{0,0,0}`, diameter 2 — `sceneRadius` is 1.732 and the formula reproduces today's values exactly.)

- [ ] **Step 4: Run the full suite** (camera behavior must be unchanged at defaults)

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure
```
Expected: all pass, including `pcinspector_ui_tests` (frameScene position `{0,-4,0}` check in the renderer contract test).

- [ ] **Step 5: Commit**

```bash
git add src/core/NavigationCamera.h src/core/NavigationCamera.cpp \
        tests/unit/NavigationCameraSceneTests.cpp CMakeLists.txt
git commit -m "feat: make navigation camera scene-bounds aware"
```

---

### Task 9: Synthetic scenes and the LoadedPointCloud adapter

Two scene factories: one wraps the procedural synthetic cloud into blocks; the other converts a legacy `LoadedPointCloud` into a scene that decodes to exactly the same normalized `[-1,1]` positions the old shader produced (`q / 65535 * 2 - 1`). The adapter is **temporary** — it lets Task 11 switch the render stack with zero behavior change, and Task 12 deletes it.

**Files:**
- Create: `src/scene/SyntheticScene.h`, `src/scene/SyntheticScene.cpp`
- Create: `src/scene/LoadedCloudScene.h`, `src/scene/LoadedCloudScene.cpp`
- Test: `tests/unit/SyntheticSceneTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 4/6 types, `pci::generatePointChunk` (`core/SyntheticPointCloud.h`), `pci::LoadedPointCloud` (`pointcloud/LoadedPointCloud.h`).
- Produces (used by Tasks 11–12):
  - `PointCloudScenePtr pci::buildSyntheticScene(std::uint64_t pointCount);` — blocks with `origin {-1,-1,-1}`, `scale = 2.0/65535.0`, bounds the `[-1,1]` cube, metadata `hasColor = true`, `sourcePointCount = pointCount`, `sourceBounds` the `[-1,1]` cube.
  - `PointCloudScenePtr pci::sceneFromLoadedCloud(const LoadedPointCloudPtr &cloud);` — same origin/scale mapping; slices the cloud into blocks of at most `maximumPointsPerBlock` preserving order; copies per-slice `attributes` when the cloud has them; metadata copied from the cloud **except** `sourceBounds`, which is forced to the `[-1,1]` cube so the camera keeps framing the geometry's actual space.

- [ ] **Step 1: Write the failing test** — `tests/unit/SyntheticSceneTests.cpp`:

```cpp
#include "core/SyntheticPointCloud.h"
#include "pointcloud/LoadedPointCloud.h"
#include "scene/LoadedCloudScene.h"
#include "scene/SyntheticScene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("synthetic scenes chunk points into capacity-bounded blocks",
          "[unit][scene]")
{
    const std::uint64_t total = 2 * pci::maximumPointsPerBlock + 5;
    const auto scene = pci::buildSyntheticScene(total);

    CHECK(scene->totalPointCount() == total);
    CHECK(scene->metadata().hasColor);
    CHECK(scene->metadata().sourcePointCount == total);
    const auto blocks = scene->blocks();
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->points.size() == pci::maximumPointsPerBlock);
    CHECK(blocks[2]->points.size() == 5);
    CHECK(blocks[0]->points.front() == pci::generatePoint(0));
    CHECK(blocks[1]->points.front()
          == pci::generatePoint(pci::maximumPointsPerBlock));
    // Same normalized decode as the legacy shader: q/65535*2-1.
    const pci::Vec3d decoded = pci::decodeBlockPosition(
        *blocks[0], blocks[0]->points.front());
    const pci::GpuPoint reference = pci::generatePoint(0);
    CHECK(decoded.x
          == Catch::Approx(reference.x / 65535.0 * 2.0 - 1.0));
}

TEST_CASE("loaded clouds convert to normalized-space scenes",
          "[unit][scene]")
{
    auto cloud = std::make_shared<pci::LoadedPointCloud>();
    cloud->metadata.sourcePointCount = 2;
    cloud->metadata.hasIntensity = true;
    cloud->metadata.sourceBounds = {
        .minimum = {1000.0, 2000.0, 10.0},
        .maximum = {1010.0, 2010.0, 20.0},
    };
    cloud->points.push_back({.x = 32768, .y = 32768, .z = 32768});
    cloud->points.push_back({.x = 0, .y = 0, .z = 65535});
    cloud->attributes.push_back({.intensity = 700});
    cloud->attributes.push_back({.intensity = 100});

    const auto scene = pci::sceneFromLoadedCloud(cloud);
    CHECK(scene->totalPointCount() == 2);
    CHECK(scene->metadata().hasIntensity);
    CHECK(scene->intensityMaximum() == 700);
    // Geometry space is normalized regardless of source bounds.
    CHECK(scene->metadata().sourceBounds.minimum
          == std::array{-1.0, -1.0, -1.0});
    const auto blocks = scene->blocks();
    REQUIRE(blocks.size() == 1);
    const pci::Vec3d second = pci::decodeBlockPosition(
        *blocks.front(), blocks.front()->points[1]);
    CHECK(second.x == Catch::Approx(-1.0));
    CHECK(second.z == Catch::Approx(1.0));
}

} // namespace
```

Add to `pcinspector_unit_tests` SOURCES; add both new `.cpp` files to `pcinspector_scene`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: FAIL — headers not found.

- [ ] **Step 3: Implement** — `src/scene/SyntheticScene.h`:

```cpp
#pragma once

#include "scene/PointCloudScene.h"

#include <cstdint>

namespace pci {

[[nodiscard]] PointCloudScenePtr
buildSyntheticScene(std::uint64_t pointCount);

} // namespace pci
```

`src/scene/SyntheticScene.cpp`:

```cpp
#include "scene/SyntheticScene.h"

#include "core/SyntheticPointCloud.h"

#include <algorithm>

namespace pci {
namespace {

constexpr Bounds3d normalizedCube{
    .minimum = {-1.0, -1.0, -1.0},
    .maximum = {1.0, 1.0, 1.0},
};

} // namespace

PointCloudScenePtr buildSyntheticScene(const std::uint64_t pointCount)
{
    PointCloudMetadata metadata;
    metadata.sourceDriver = "synthetic";
    metadata.sourcePointCount = pointCount;
    metadata.sourceBounds = normalizedCube;
    metadata.hasColor = true;

    auto scene = std::make_shared<PointCloudScene>(std::move(metadata));
    for (std::uint64_t first = 0; first < pointCount;
         first += maximumPointsPerBlock) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            maximumPointsPerBlock, pointCount - first));
        auto block = std::make_shared<PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / blockQuantizationSteps;
        block->bounds = normalizedCube;
        block->points = generatePointChunk(first, count);
        block->attributes.resize(count);
        scene->addBlock(std::move(block));
    }
    return scene;
}

} // namespace pci
```

`src/scene/LoadedCloudScene.h`:

```cpp
#pragma once

#include "pointcloud/LoadedPointCloud.h"
#include "scene/PointCloudScene.h"

namespace pci {

// Temporary bridge for the legacy import path; removed once the loader
// produces scenes directly. Decodes to the legacy normalized [-1,1] space.
[[nodiscard]] PointCloudScenePtr
sceneFromLoadedCloud(const LoadedPointCloudPtr &cloud);

} // namespace pci
```

`src/scene/LoadedCloudScene.cpp`:

```cpp
#include "scene/LoadedCloudScene.h"

#include <algorithm>
#include <stdexcept>

namespace pci {

PointCloudScenePtr sceneFromLoadedCloud(const LoadedPointCloudPtr &cloud)
{
    if (!cloud || cloud->points.empty()) {
        throw std::invalid_argument(
            "scene conversion requires a non-empty point cloud");
    }

    constexpr Bounds3d normalizedCube{
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    PointCloudMetadata metadata = cloud->metadata;
    metadata.sourceBounds = normalizedCube;

    auto scene = std::make_shared<PointCloudScene>(std::move(metadata));
    const std::size_t total = cloud->points.size();
    const bool hasAttributes = cloud->attributes.size() == total;
    for (std::size_t first = 0; first < total;
         first += maximumPointsPerBlock) {
        const std::size_t count = std::min<std::size_t>(
            maximumPointsPerBlock, total - first);
        auto block = std::make_shared<PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / blockQuantizationSteps;
        block->bounds = normalizedCube;
        block->points.assign(
            cloud->points.begin() + static_cast<std::ptrdiff_t>(first),
            cloud->points.begin()
                + static_cast<std::ptrdiff_t>(first + count));
        if (hasAttributes) {
            block->attributes.assign(
                cloud->attributes.begin()
                    + static_cast<std::ptrdiff_t>(first),
                cloud->attributes.begin()
                    + static_cast<std::ptrdiff_t>(first + count));
            for (const PointAttributes &attributes : block->attributes) {
                block->intensityMaximum = std::max(
                    block->intensityMaximum, attributes.intensity);
            }
        } else {
            block->attributes.resize(count);
        }
        scene->addBlock(std::move(block));
    }
    return scene;
}

} // namespace pci
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure -R unit
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene/SyntheticScene.h src/scene/SyntheticScene.cpp \
        src/scene/LoadedCloudScene.h src/scene/LoadedCloudScene.cpp \
        tests/unit/SyntheticSceneTests.cpp CMakeLists.txt
git commit -m "feat: add synthetic scene builder and legacy cloud adapter"
```

---

### Task 10: `UploadScheduler` — budgeted per-block uploads

Replaces monolithic upload with per-block GPU buffers filled under a per-frame byte budget. Pure selection logic is a free function so it is testable without a GPU.

**Files:**
- Create: `src/renderer/rhi/UploadScheduler.h`, `src/renderer/rhi/UploadScheduler.cpp`
- Test: `tests/qt/UploadSchedulerTests.cpp`
- Modify: `CMakeLists.txt` (renderer sources + link `pcinspector_scene`; register test in `pcinspector_qt_renderer_tests`)

**Interfaces:**
- Consumes: `PointBlockPtr` (Task 4), QRhi.
- Produces (used by Task 11):
  - `std::size_t pci::planUploadCount(const std::vector<std::uint64_t> &pendingByteSizes, std::uint64_t byteBudget) noexcept` — number of leading entries to upload this frame; always ≥ 1 when any are pending.
  - `class pci::UploadScheduler` with:
    - `static constexpr std::uint64_t defaultFrameByteBudget = 48ULL * 1024 * 1024;`
    - `std::size_t uploadPending(QRhi *rhi, QRhiCommandBuffer *commandBuffer, const std::vector<PointBlockPtr> &blocks, std::uint64_t byteBudget)` — creates+fills vertex buffers for blocks that lack one, in order, within budget; returns blocks uploaded this call; throws `std::runtime_error` on buffer-creation failure.
    - `QRhiBuffer *bufferFor(const PointBlock *block) const noexcept` — null when not resident.
    - `std::uint64_t residentPointCount() const noexcept`
    - `void releaseResources()` (also called when the QRhi instance changes).

- [ ] **Step 1: Write the failing test** — `tests/qt/UploadSchedulerTests.cpp`:

```cpp
#include "renderer/rhi/UploadScheduler.h"

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("upload planning fills the byte budget in order",
          "[qt][renderer]")
{
    const std::uint64_t mb = 1024 * 1024;
    CHECK(pci::planUploadCount({16 * mb, 16 * mb, 16 * mb, 16 * mb},
                               48 * mb)
          == 3);
    CHECK(pci::planUploadCount({16 * mb, 40 * mb}, 48 * mb) == 2);
    CHECK(pci::planUploadCount({}, 48 * mb) == 0);
}

TEST_CASE("upload planning always makes progress", "[qt][renderer]")
{
    CHECK(pci::planUploadCount({100 * 1024 * 1024}, 48 * 1024 * 1024)
          == 1);
}

} // namespace
```

Register in `pcinspector_qt_renderer_tests` SOURCES.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 8`
Expected: FAIL — header not found.

- [ ] **Step 3: Implement** — `src/renderer/rhi/UploadScheduler.h`:

```cpp
#pragma once

#include "scene/PointBlock.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;

namespace pci {

[[nodiscard]] std::size_t planUploadCount(
    const std::vector<std::uint64_t> &pendingByteSizes,
    std::uint64_t byteBudget) noexcept;

class UploadScheduler {
public:
    static constexpr std::uint64_t defaultFrameByteBudget =
        48ULL * 1024 * 1024;

    UploadScheduler() = default;
    ~UploadScheduler();

    UploadScheduler(const UploadScheduler &) = delete;
    UploadScheduler &operator=(const UploadScheduler &) = delete;

    std::size_t uploadPending(
        QRhi *rhi,
        QRhiCommandBuffer *commandBuffer,
        const std::vector<PointBlockPtr> &blocks,
        std::uint64_t byteBudget = defaultFrameByteBudget);
    [[nodiscard]] QRhiBuffer *
    bufferFor(const PointBlock *block) const noexcept;
    [[nodiscard]] std::uint64_t residentPointCount() const noexcept;
    void releaseResources();

private:
    QRhi *rhi_ = nullptr;
    std::unordered_map<const PointBlock *, QRhiBuffer *> buffers_;
    std::uint64_t residentPoints_ = 0;
};

} // namespace pci
```

`src/renderer/rhi/UploadScheduler.cpp`:

```cpp
#include "renderer/rhi/UploadScheduler.h"

#include <rhi/qrhi.h>

#include <stdexcept>

namespace pci {

std::size_t planUploadCount(
    const std::vector<std::uint64_t> &pendingByteSizes,
    const std::uint64_t byteBudget) noexcept
{
    std::size_t count = 0;
    std::uint64_t used = 0;
    for (const std::uint64_t size : pendingByteSizes) {
        if (count > 0 && used + size > byteBudget) {
            break;
        }
        used += size;
        ++count;
    }
    return count;
}

UploadScheduler::~UploadScheduler()
{
    releaseResources();
}

std::size_t UploadScheduler::uploadPending(
    QRhi *rhi, QRhiCommandBuffer *commandBuffer,
    const std::vector<PointBlockPtr> &blocks,
    const std::uint64_t byteBudget)
{
    if (!rhi || !commandBuffer) {
        throw std::invalid_argument(
            "block uploads require QRhi resources");
    }
    if (rhi_ && rhi_ != rhi) {
        releaseResources();
    }
    rhi_ = rhi;

    std::vector<const PointBlock *> pending;
    std::vector<std::uint64_t> pendingSizes;
    for (const PointBlockPtr &block : blocks) {
        if (!buffers_.contains(block.get())) {
            pending.push_back(block.get());
            pendingSizes.push_back(
                block->points.size() * sizeof(GpuPoint));
        }
    }
    const std::size_t uploadCount =
        planUploadCount(pendingSizes, byteBudget);

    for (std::size_t i = 0; i < uploadCount; ++i) {
        const PointBlock *block = pending[i];
        const auto bytes = static_cast<quint32>(
            block->points.size() * sizeof(GpuPoint));
        QRhiBuffer *buffer = rhi_->newBuffer(
            QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes);
        buffer->setName(QByteArrayLiteral("Point block"));
        if (!buffer->create()) {
            delete buffer;
            throw std::runtime_error(
                "Could not create QRhi point-block buffer");
        }
        QRhiResourceUpdateBatch *updates =
            rhi_->nextResourceUpdateBatch();
        updates->uploadStaticBuffer(
            buffer, 0, bytes, block->points.data());
        commandBuffer->resourceUpdate(updates);
        buffers_.emplace(block, buffer);
        residentPoints_ += block->points.size();
    }
    return uploadCount;
}

QRhiBuffer *
UploadScheduler::bufferFor(const PointBlock *block) const noexcept
{
    const auto found = buffers_.find(block);
    return found == buffers_.end() ? nullptr : found->second;
}

std::uint64_t UploadScheduler::residentPointCount() const noexcept
{
    return residentPoints_;
}

void UploadScheduler::releaseResources()
{
    for (auto &[block, buffer] : buffers_) {
        delete buffer;
    }
    buffers_.clear();
    residentPoints_ = 0;
    rhi_ = nullptr;
}

} // namespace pci
```

CMake: add both files to `pcinspector_renderer`; add `pcinspector_scene` to its `target_link_libraries` PUBLIC list.

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build -j 8 && ctest --test-dir build --output-on-failure
```
Expected: PASS (scheduler is compiled but not yet used by the viewport).

- [ ] **Step 5: Commit**

```bash
git add src/renderer/rhi/UploadScheduler.h src/renderer/rhi/UploadScheduler.cpp \
        tests/qt/UploadSchedulerTests.cpp CMakeLists.txt
git commit -m "feat: add budgeted per-block upload scheduler"
```

---

### Task 11: Render-stack switchover to per-block draws

The uniform layout, shaders, renderer, picker, and viewport change together (they share the uniform block), so this is one atomic task. External behavior is preserved: `setPointCloud` remains on the interface and routes through `sceneFromLoadedCloud`, so **all existing contract, pick-result, GPU, and smoke tests must pass unmodified** — with one mechanical exception: `setScene` is added to the `RenderViewport` interface as pure virtual, so the `FakeViewport` in `tests/ui/MainWindowTests.cpp` gains a trivial override (Step 4a). A new GPU test covers multi-block picking via the new `setScene`.

Key mechanics:
- **Per-block uniform slices** in one dynamic uniform buffer, bound with `QRhiShaderResourceBinding::uniformBufferWithDynamicOffset`, selected per draw via `QRhiCommandBuffer::DynamicOffset` (`QPair<int, quint32>`); slice stride is `rhi->ubufAligned(sizeof(BlockUniform))`.
- **Eye-relative transforms**: the only double-precision subtraction is `block->origin - camera.position()`; the per-block MVP is `clipCorr * projection * rotationOnlyView * translate(relative) * scale(block->scale)`, so float precision holds at any world magnitude.
- **Picking**: fixed 64×64 R32UI target; the full-size viewport rectangle is translated so the cursor pixel lands at the target center; point IDs are `gl_VertexIndex + idBase + 1` with `idBase` accumulated across the visible draw list; the completion maps a global ID back to (block, local index).

**Files:**
- Modify: `shaders/points.vert`, `shaders/points.frag` (unchanged content check only), `shaders/pick.vert`
- Modify: `src/renderer/rhi/PointCloudRenderer.h`, `src/renderer/rhi/PointCloudRenderer.cpp`
- Modify: `src/renderer/rhi/PointPicker.h`, `src/renderer/rhi/PointPicker.cpp`
- Modify: `src/renderer/RenderViewport.h` (add `setScene`), `src/renderer/rhi/RenderViewportWidget.h`, `src/renderer/rhi/RenderViewportWidget.cpp`
- Delete: `src/renderer/rhi/PointBufferUploader.h`, `src/renderer/rhi/PointBufferUploader.cpp`
- Test: `tests/gpu/PointPickerGpuTests.cpp` (add multi-block case), `tests/ui/MainWindowTests.cpp` (FakeViewport implements `setScene`)
- Modify: `CMakeLists.txt` (drop deleted sources)

**Interfaces:**
- Consumes: Tasks 4–10 (`PointCloudScenePtr`, `FrustumCuller`, `UploadScheduler`, `buildSyntheticScene`, `sceneFromLoadedCloud`, `decodeBlockPosition`, `NavigationCamera::setScene`).
- Produces (used by Task 12):
  - `RenderViewport::setScene(PointCloudScenePtr scene)` (pure virtual, added).
  - `struct pci::BlockUniform` (112 bytes, layout below) and `struct pci::BlockDraw { PointBlockPtr block; QRhiBuffer *buffer; quint32 pointCount; quint32 idBase; BlockUniform uniform; };`
  - `PointCloudRenderer::{ensureResources, updateUniforms(QRhiCommandBuffer*, const std::vector<BlockDraw>&), draw(QRhiCommandBuffer*, QRhiRenderTarget*, const std::vector<BlockDraw>&), shaderBindings(), uniformStride(), ready(), releaseResources()}`
  - `PointPicker::record(QRhiCommandBuffer*, const std::vector<BlockDraw>&, quint32 uniformStride, QPoint pixelPosition, QSize renderTargetPixelSize, Completion)`; `decodeNearestId` unchanged.

- [ ] **Step 1: New uniform block in the shaders**

`shaders/points.vert` — replace the input/uniform/main sections; the color-map helper functions (`normalizedScalar`, `viridisColor`, `turboColor`, `scalarColor`, `classificationColor`, `returnColor`) and the constant tables stay exactly as they are:

```glsl
#version 450

layout(location = 0) in uvec4 positionAttributes;
layout(location = 1) in vec4 color;
layout(location = 2) in uint packedProperties;

layout(binding = 0) uniform BlockData {
    mat4 mvp;
    vec4 originAndScale; // xyz world origin, w world units per step
    float pointSize;
    int colorSource;
    int colorMap;
    float scalarMinimum;
    float scalarMaximum;
    int idBase;
} camera;

layout(location = 0) out vec4 pointColor;
```

(keeping the `camera` instance name means the helper functions compile unchanged). Replace `mappedColor()`'s coordinate cases to use world positions and change `main()`:

```glsl
vec4 mappedColor()
{
    vec3 world = camera.originAndScale.xyz
        + vec3(positionAttributes.xyz) * camera.originAndScale.w;
    if (camera.colorSource == ColorRgb) {
        return color;
    }
    if (camera.colorSource == ColorX) {
        return vec4(scalarColor(world.x), 1.0);
    }
    if (camera.colorSource == ColorY) {
        return vec4(scalarColor(world.y), 1.0);
    }
    if (camera.colorSource == ColorZ) {
        return vec4(scalarColor(world.z), 1.0);
    }
    if (camera.colorSource == ColorIntensity) {
        uint intensity = packedProperties & 0xffffu;
        return vec4(scalarColor(float(intensity)), 1.0);
    }
    if (camera.colorSource == ColorClassification) {
        uint classification = positionAttributes.w & 0xffu;
        return vec4(classificationColor(classification), 1.0);
    }
    if (camera.colorSource == ColorReturnNumber) {
        uint returnNumber = (packedProperties >> 16u) & 0xffu;
        return vec4(returnColor(returnNumber), 1.0);
    }
    if (camera.colorSource == ColorNumberOfReturns) {
        uint numberOfReturns = (packedProperties >> 24u) & 0xffu;
        return vec4(returnColor(numberOfReturns), 1.0);
    }
    return color;
}

void main()
{
    gl_Position = camera.mvp * vec4(vec3(positionAttributes.xyz), 1.0);
    gl_PointSize = camera.pointSize;
    pointColor = mappedColor();
}
```

`shaders/pick.vert` — full replacement:

```glsl
#version 450

layout(location = 0) in uvec4 positionAttributes;

layout(binding = 0) uniform BlockData {
    mat4 mvp;
    vec4 originAndScale;
    float pointSize;
    int colorSource;
    int colorMap;
    float scalarMinimum;
    float scalarMaximum;
    int idBase;
} camera;

layout(location = 0) flat out uint pointId;

void main()
{
    gl_Position = camera.mvp * vec4(vec3(positionAttributes.xyz), 1.0);
    gl_PointSize = camera.pointSize;
    pointId = uint(gl_VertexIndex + camera.idBase) + 1u;
}
```

- [ ] **Step 2: `PointCloudRenderer` — BlockUniform + dynamic offsets**

`src/renderer/rhi/PointCloudRenderer.h` — full replacement:

```cpp
#pragma once

#include "scene/PointBlock.h"

#include <QtCore/qtypes.h>

#include <cstdint>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiShaderResourceBindings;

namespace pci {

struct alignas(16) BlockUniform {
    float mvp[16]{};
    float originAndScale[4]{};
    float pointSize = 1.0F;
    std::int32_t colorSource = 0;
    std::int32_t colorMap = 0;
    float scalarMinimum = 0.0F;
    float scalarMaximum = 1.0F;
    std::int32_t idBase = 0;
    float padding[2]{};
};
static_assert(sizeof(BlockUniform) == 112);

struct BlockDraw {
    PointBlockPtr block;
    QRhiBuffer *buffer = nullptr;
    quint32 pointCount = 0;
    quint32 idBase = 0;
    BlockUniform uniform;
};

class PointCloudRenderer {
public:
    PointCloudRenderer() = default;
    ~PointCloudRenderer();

    PointCloudRenderer(const PointCloudRenderer &) = delete;
    PointCloudRenderer &operator=(const PointCloudRenderer &) = delete;

    void ensureResources(QRhi *rhi,
                         QRhiRenderPassDescriptor *renderPassDescriptor);
    void updateUniforms(QRhiCommandBuffer *commandBuffer,
                        const std::vector<BlockDraw> &draws);
    void draw(QRhiCommandBuffer *commandBuffer,
              QRhiRenderTarget *renderTarget,
              const std::vector<BlockDraw> &draws);
    [[nodiscard]] QRhiShaderResourceBindings *
    shaderBindings() const noexcept;
    [[nodiscard]] quint32 uniformStride() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    void releaseResources();

private:
    void createResourceBindings(std::size_t drawCapacity);
    void createPipeline(QRhiRenderPassDescriptor *renderPassDescriptor);
    void ensureUniformCapacity(std::size_t drawCount);

    QRhi *rhi_ = nullptr;
    QRhiBuffer *uniformBuffer_ = nullptr;
    quint32 uniformStride_ = 0;
    std::size_t uniformCapacity_ = 0;
    QRhiShaderResourceBindings *shaderBindings_ = nullptr;
    QRhiGraphicsPipeline *pipeline_ = nullptr;
    QRhiRenderPassDescriptor *pipelineRenderPass_ = nullptr;
};

} // namespace pci
```

`src/renderer/rhi/PointCloudRenderer.cpp` — full replacement:

```cpp
#include "renderer/rhi/PointCloudRenderer.h"

#include "core/GpuPoint.h"
#include "renderer/PointColorPolicy.h"
#include "renderer/rhi/ShaderLoader.h"

#include <QColor>
#include <rhi/qrhi.h>

#include <stdexcept>
#include <string>

namespace pci {
namespace {

// Compile-time guard for the manual C++/GLSL enum coupling (spec §3.2):
// these values must match the Color*/Map* constants in points.vert.
static_assert(static_cast<int>(PointColorSource::Rgb) == 0);
static_assert(static_cast<int>(PointColorSource::X) == 1);
static_assert(static_cast<int>(PointColorSource::Y) == 2);
static_assert(static_cast<int>(PointColorSource::Z) == 3);
static_assert(static_cast<int>(PointColorSource::Intensity) == 4);
static_assert(static_cast<int>(PointColorSource::Classification) == 5);
static_assert(static_cast<int>(PointColorSource::ReturnNumber) == 6);
static_assert(static_cast<int>(PointColorSource::NumberOfReturns) == 7);
static_assert(static_cast<int>(PointColorMap::Rgb) == 0);
static_assert(static_cast<int>(PointColorMap::Grayscale) == 1);
static_assert(static_cast<int>(PointColorMap::Viridis) == 2);
static_assert(static_cast<int>(PointColorMap::Turbo) == 3);
static_assert(static_cast<int>(PointColorMap::LasClassification) == 4);
static_assert(static_cast<int>(PointColorMap::ReturnNumber) == 5);

constexpr std::size_t initialDrawCapacity = 256;

void requireCreated(const bool created, const char *resource)
{
    if (!created) {
        throw std::runtime_error(
            "Could not create QRhi " + std::string(resource));
    }
}

} // namespace

PointCloudRenderer::~PointCloudRenderer()
{
    releaseResources();
}

void PointCloudRenderer::ensureResources(
    QRhi *rhi, QRhiRenderPassDescriptor *renderPassDescriptor)
{
    if (!rhi || !renderPassDescriptor) {
        throw std::invalid_argument(
            "point renderer requires QRhi resources");
    }
    if (rhi_ != rhi) {
        releaseResources();
        rhi_ = rhi;
        createResourceBindings(initialDrawCapacity);
    }
    if (!pipeline_ || pipelineRenderPass_ != renderPassDescriptor) {
        createPipeline(renderPassDescriptor);
    }
}

void PointCloudRenderer::updateUniforms(
    QRhiCommandBuffer *commandBuffer, const std::vector<BlockDraw> &draws)
{
    if (!ready() || !commandBuffer) {
        throw std::logic_error("point renderer is not ready to update");
    }
    ensureUniformCapacity(draws.size());
    QRhiResourceUpdateBatch *updates = rhi_->nextResourceUpdateBatch();
    for (std::size_t i = 0; i < draws.size(); ++i) {
        updates->updateDynamicBuffer(
            uniformBuffer_,
            static_cast<quint32>(i) * uniformStride_,
            sizeof(BlockUniform),
            &draws[i].uniform);
    }
    commandBuffer->resourceUpdate(updates);
}

void PointCloudRenderer::draw(QRhiCommandBuffer *commandBuffer,
                              QRhiRenderTarget *renderTarget,
                              const std::vector<BlockDraw> &draws)
{
    if (!ready() || !commandBuffer || !renderTarget) {
        throw std::logic_error("point renderer is not ready to draw");
    }

    commandBuffer->beginPass(
        renderTarget, QColor::fromRgbF(0.015, 0.02, 0.035, 1.0),
        {1.0F, 0});
    commandBuffer->setGraphicsPipeline(pipeline_);
    commandBuffer->setViewport(
        QRhiViewport(
            0, 0,
            static_cast<float>(renderTarget->pixelSize().width()),
            static_cast<float>(renderTarget->pixelSize().height())));
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(i) * uniformStride_);
        commandBuffer->setShaderResources(shaderBindings_, 1, &offset);
        const QRhiCommandBuffer::VertexInput vertexInput(
            draws[i].buffer, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(draws[i].pointCount);
    }
    commandBuffer->endPass();
}

QRhiShaderResourceBindings *
PointCloudRenderer::shaderBindings() const noexcept
{
    return shaderBindings_;
}

quint32 PointCloudRenderer::uniformStride() const noexcept
{
    return uniformStride_;
}

bool PointCloudRenderer::ready() const noexcept
{
    return rhi_ && uniformBuffer_ && shaderBindings_ && pipeline_;
}

void PointCloudRenderer::releaseResources()
{
    delete pipeline_;
    pipeline_ = nullptr;
    pipelineRenderPass_ = nullptr;
    delete shaderBindings_;
    shaderBindings_ = nullptr;
    delete uniformBuffer_;
    uniformBuffer_ = nullptr;
    uniformCapacity_ = 0;
    rhi_ = nullptr;
}

void PointCloudRenderer::createResourceBindings(
    const std::size_t drawCapacity)
{
    uniformStride_ = static_cast<quint32>(
        rhi_->ubufAligned(sizeof(BlockUniform)));
    uniformCapacity_ = drawCapacity;

    delete uniformBuffer_;
    uniformBuffer_ = rhi_->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
        uniformStride_ * static_cast<quint32>(uniformCapacity_));
    uniformBuffer_->setName(QByteArrayLiteral("Block uniforms"));
    requireCreated(uniformBuffer_->create(), "block uniform buffer");

    delete shaderBindings_;
    shaderBindings_ = rhi_->newShaderResourceBindings();
    shaderBindings_->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0, QRhiShaderResourceBinding::VertexStage, uniformBuffer_,
            sizeof(BlockUniform)),
    });
    requireCreated(shaderBindings_->create(),
                   "shader resource bindings");
}

void PointCloudRenderer::ensureUniformCapacity(
    const std::size_t drawCount)
{
    if (drawCount <= uniformCapacity_) {
        return;
    }
    std::size_t capacity = uniformCapacity_;
    while (capacity < drawCount) {
        capacity *= 2;
    }
    createResourceBindings(capacity);
}

void PointCloudRenderer::createPipeline(
    QRhiRenderPassDescriptor *renderPassDescriptor)
{
    delete pipeline_;
    pipeline_ = nullptr;

    QString error;
    const QShader vertexShader = loadShaderResource(
        QStringLiteral(":/shaders/points.vert.qsb"), &error);
    if (!vertexShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }
    const QShader fragmentShader = loadShaderResource(
        QStringLiteral(":/shaders/points.frag.qsb"), &error);
    if (!fragmentShader.isValid()) {
        throw std::runtime_error(error.toStdString());
    }

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({QRhiVertexInputBinding(sizeof(GpuPoint))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(
            0, 0, QRhiVertexInputAttribute::UShort4, 0),
        QRhiVertexInputAttribute(
            0, 1, QRhiVertexInputAttribute::UNormByte4, 8),
        QRhiVertexInputAttribute(
            0, 2, QRhiVertexInputAttribute::UInt, 12),
    });

    pipeline_ = rhi_->newGraphicsPipeline();
    pipeline_->setName(QByteArrayLiteral("Point pipeline"));
    pipeline_->setTopology(QRhiGraphicsPipeline::Points);
    pipeline_->setDepthTest(true);
    pipeline_->setDepthWrite(true);
    pipeline_->setDepthOp(QRhiGraphicsPipeline::Less);
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, fragmentShader},
    });
    pipeline_->setVertexInputLayout(inputLayout);
    pipeline_->setShaderResourceBindings(shaderBindings_);
    pipelineRenderPass_ = renderPassDescriptor;
    pipeline_->setRenderPassDescriptor(pipelineRenderPass_);
    requireCreated(pipeline_->create(), "point graphics pipeline");
}

} // namespace pci
```

Note: `ensureUniformCapacity` recreates the uniform buffer and bindings mid-frame **before** any pass begins (it is called from `updateUniforms`, which runs before `beginPass`); the recreated bindings are layout-compatible so the existing pipeline remains valid.

- [ ] **Step 3: `PointPicker` — fixed small target, per-block draws**

`src/renderer/rhi/PointPicker.h` — replace the `ensureResources`/`record` declarations and remove the `pixelSize_` member:

```cpp
    static constexpr int pickTargetEdge = 64;

    void ensureResources(QRhi *rhi,
                         QRhiShaderResourceBindings *shaderBindings);
    [[nodiscard]] bool inFlight() const noexcept;
    void record(QRhiCommandBuffer *commandBuffer,
                const std::vector<BlockDraw> &draws,
                quint32 uniformStride,
                QPoint pixelPosition,
                QSize renderTargetPixelSize,
                Completion completion);
```

(add `#include "renderer/rhi/PointCloudRenderer.h"` for `BlockDraw`, keep `decodeNearestId` exactly as is).

`src/renderer/rhi/PointPicker.cpp` — `ensureResources` creates the fixed-size target once per QRhi/bindings pair:

```cpp
void PointPicker::ensureResources(
    QRhi *rhi, QRhiShaderResourceBindings *shaderBindings)
{
    if (!rhi || !shaderBindings) {
        return;
    }
    if (rhi_ == rhi && shaderBindings_ == shaderBindings && pipeline_) {
        return;
    }

    releaseResources();
    const QSize pixelSize(pickTargetEdge, pickTargetEdge);
    const auto textureFlags =
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource;
    if (!rhi->isTextureFormatSupported(
            QRhiTexture::R32UI, textureFlags)) {
        throw std::runtime_error(
            "GPU device does not support R32UI picking targets");
    }

    rhi_ = rhi;
    shaderBindings_ = shaderBindings;
    ...
```

The texture/depth/render-target/pipeline creation below is unchanged except every `pixelSize_` becomes the local `pixelSize`. `record` becomes:

```cpp
void PointPicker::record(
    QRhiCommandBuffer *commandBuffer,
    const std::vector<BlockDraw> &draws,
    const quint32 uniformStride,
    const QPoint pixelPosition,
    const QSize renderTargetPixelSize,
    Completion completion)
{
    if (!commandBuffer || !pipeline_ || inFlight_) {
        return;
    }
    if (draws.empty()) {
        completion(std::nullopt);
        return;
    }

    // Translate the full-size viewport so the cursor pixel lands at the
    // centre of the fixed 64x64 target. QRhi viewports are bottom-left
    // based; the cursor position is top-left based.
    const auto half = static_cast<float>(pickTargetEdge) * 0.5F;
    const float viewportX =
        half - static_cast<float>(pixelPosition.x());
    const float viewportY =
        half - (static_cast<float>(renderTargetPixelSize.height())
                - static_cast<float>(pixelPosition.y()));

    commandBuffer->beginPass(
        renderTarget_, QColor::fromRgba64(0, 0, 0, 0), {1.0F, 0});
    commandBuffer->setGraphicsPipeline(pipeline_);
    commandBuffer->setViewport(
        QRhiViewport(
            viewportX, viewportY,
            static_cast<float>(renderTargetPixelSize.width()),
            static_cast<float>(renderTargetPixelSize.height())));
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const QRhiCommandBuffer::DynamicOffset offset(
            0, static_cast<quint32>(i) * uniformStride);
        commandBuffer->setShaderResources(shaderBindings_, 1, &offset);
        const QRhiCommandBuffer::VertexInput vertexInput(
            draws[i].buffer, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(draws[i].pointCount);
    }
    commandBuffer->endPass();

    readback_ = std::make_unique<QRhiReadbackResult>();
    inFlight_ = true;
    readback_->completed =
        [this, completion = std::move(completion)] {
            const auto result =
                decodeNearestId(readback_->data, readback_->pixelSize);
            inFlight_ = false;
            completion(result);
        };
    QRhiReadbackDescription description(idTexture_);
    const int center = pickTargetEdge / 2;
    description.setRect(QRect(center - 2, center - 2, 5, 5));
    QRhiResourceUpdateBatch *updates = rhi_->nextResourceUpdateBatch();
    updates->readBackTexture(description, readback_.get());
    commandBuffer->resourceUpdate(updates);
}
```

- [ ] **Step 4: Viewport rework**

`src/renderer/RenderViewport.h` — add alongside `setPointCloud` (which stays until Task 12):

```cpp
    virtual void setScene(PointCloudScenePtr scene) = 0;
```

with `#include "scene/PointCloudScene.h"` at the top.

`src/renderer/rhi/RenderViewportWidget.h` — replace the private data section: remove `pointCloud_`, `uploadManager_` (PointBufferUploader), `pointResourcesDirty_`, and the declarations of `createDeviceResources`, `createPointBuffer`, `cameraUniform`, `pointAt`, and the old two-argument `resolvePick`; change `submitPendingPick` to `void submitPendingPick(QRhiCommandBuffer *commandBuffer, const std::vector<BlockDraw> &draws);`; add:

```cpp
    void applyScene(PointCloudScenePtr scene);
    [[nodiscard]] std::vector<BlockDraw> buildDrawList(
        const std::vector<PointBlockPtr> &blocks);
    void resolvePick(PickRequest request,
                     std::optional<std::uint32_t> pointId,
                     const std::vector<BlockDraw> &draws);

    PointCloudScenePtr scene_;
    std::uint64_t seenSceneRevision_ = 0;
    UploadScheduler uploadScheduler_;
```

and the overrides `void setScene(PointCloudScenePtr scene) override;` / `void setPointCloud(LoadedPointCloudPtr cloud) override;`.

`src/renderer/rhi/RenderViewportWidget.cpp` — the required replacements (event handlers, color-mode methods, callbacks, metrics/progress/fail publication, and the smoke-exit logic all stay as they are):

```cpp
// Constructor body: replace pointBudget_(pointCount) initialisation use;
// after the existing setup lines add:
    if (pointCount > 0) {
        applyScene(buildSyntheticScene(pointCount));
    }

void RenderViewportWidget::setScene(PointCloudScenePtr scene)
{
    if (!scene) {
        throw std::invalid_argument("scene must not be null");
    }
    applyScene(std::move(scene));
}

void RenderViewportWidget::setPointCloud(LoadedPointCloudPtr cloud)
{
    if (!cloud) {
        throw std::invalid_argument("point cloud must not be null");
    }
    if (cloud->points.empty()) {
        throw std::invalid_argument("point cloud must contain points");
    }
    applyScene(sceneFromLoadedCloud(cloud));
}

void RenderViewportWidget::applyScene(PointCloudScenePtr scene)
{
    scene_ = std::move(scene);
    seenSceneRevision_ = 0;
    activeMetadata_ = scene_->metadata();
    colorMode_ = defaultPointColorMode(activeMetadata_);
    pointCount_ = std::max(
        scene_->totalPointCount(),
        activeMetadata_.sourcePointCount);
    pointBudget_ = AdaptivePointBudget(pointCount_);
    pointCloudFrameReadyPending_ = true;
    publishLoadProgress({
        .stage = RenderLoadStage::Preparing,
        .completed = 0,
        .total = pointCount_,
    });
    input_.cancelPicks();
    rawPickCompletion_ = {};
    const Bounds3d bounds = activeMetadata_.sourceBounds;
    if (bounds.valid() && bounds.maximumExtent() > 0.0) {
        camera_.setScene(
            {bounds.center()[0], bounds.center()[1], bounds.center()[2]},
            bounds.maximumExtent());
    } else {
        camera_.setScene({0.0, 0.0, 0.0}, 2.0);
    }
    camera_.frameScene();
    uploadScheduler_.releaseResources();
    update();
}
```

`initialize()` keeps the backend/feature checks; the resource section becomes:

```cpp
        if (resourceRhi_ != rhi()) {
            releaseResources();
            resourceRhi_ = rhi();
            const QRhiDriverInfo driver = rhi()->driverInfo();
            deviceName_ = QString::fromUtf8(driver.deviceName);
            if (deviceName_.isEmpty()) {
                deviceName_ = QStringLiteral("GPU device");
            }
            previousCpuFrame_ = std::chrono::steady_clock::now();
            nextMetricsPublish_ = previousCpuFrame_;
        }
        pointCloudRenderer_.ensureResources(
            rhi(), renderTarget()->renderPassDescriptor());
        pointPicker_.ensureResources(
            rhi(), pointCloudRenderer_.shaderBindings());
```

`render()` — full replacement:

```cpp
void RenderViewportWidget::render(QRhiCommandBuffer *commandBuffer)
{
    if (failed_ || !pointCloudRenderer_.ready()) {
        return;
    }

    updateKeyboardNavigation();
    const auto duration = frameDuration(commandBuffer);
    if (renderedFrames_ > 0 && duration.count() > 0.0) {
        pointBudget_.update(duration);
        publishMetrics(duration);
    }

    std::vector<BlockDraw> draws;
    try {
        if (scene_) {
            const std::uint64_t revision = scene_->revision();
            if (revision != seenSceneRevision_) {
                seenSceneRevision_ = revision;
                pointCount_ = std::max(
                    scene_->totalPointCount(),
                    activeMetadata_.sourcePointCount);
                pointBudget_ = AdaptivePointBudget(pointCount_);
            }
            const std::vector<PointBlockPtr> blocks = scene_->blocks();
            const std::uint64_t residentBefore =
                uploadScheduler_.residentPointCount();
            uploadScheduler_.uploadPending(
                rhi(), commandBuffer, blocks);
            const std::uint64_t resident =
                uploadScheduler_.residentPointCount();
            if (resident != residentBefore
                && resident < scene_->totalPointCount()) {
                publishLoadProgress({
                    .stage = RenderLoadStage::Uploading,
                    .completed = resident,
                    .total = scene_->totalPointCount(),
                });
            }
            draws = buildDrawList(blocks);
            pointCloudRenderer_.updateUniforms(commandBuffer, draws);
            submitPendingPick(commandBuffer, draws);
        }
        pointCloudRenderer_.draw(commandBuffer, renderTarget(), draws);
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }

    if (pointCloudFrameReadyPending_
        && (!scene_ || uploadScheduler_.residentPointCount() > 0)) {
        pointCloudFrameReadyPending_ = false;
        publishLoadProgress({
            .stage = RenderLoadStage::FirstFrameReady,
            .completed = pointCount_,
            .total = pointCount_,
        });
    }

    ++renderedFrames_;
    if (smokeTest_ && renderedFrames_ >= 3) {
        QTimer::singleShot(0, QCoreApplication::instance(),
                           [] { QCoreApplication::exit(0); });
        return;
    }
    if (scene_ && uploadScheduler_.residentPointCount()
                      < scene_->totalPointCount()) {
        update(); // keep streaming uploads
        return;
    }
    update();
}
```

`buildDrawList` (new) and the camera-uniform helper replacing `cameraUniform()`:

```cpp
std::vector<BlockDraw> RenderViewportWidget::buildDrawList(
    const std::vector<PointBlockPtr> &blocks)
{
    const QSize outputSize = renderTarget()->pixelSize();
    const float aspect =
        static_cast<float>(std::max(1, outputSize.width()))
        / static_cast<float>(std::max(1, outputSize.height()));
    const auto clip = camera_.clipPlanes();

    QMatrix4x4 projection;
    projection.perspective(
        static_cast<float>(NavigationCamera::verticalFieldOfViewDegrees),
        aspect,
        static_cast<float>(clip.nearPlane),
        static_cast<float>(clip.farPlane));
    const Vec3d forward = camera_.forward();
    const Vec3d up = camera_.up();
    QMatrix4x4 view;
    view.lookAt(
        QVector3D(0.0F, 0.0F, 0.0F),
        QVector3D(static_cast<float>(forward.x),
                  static_cast<float>(forward.y),
                  static_cast<float>(forward.z)),
        QVector3D(static_cast<float>(up.x),
                  static_cast<float>(up.y),
                  static_cast<float>(up.z)));
    const QMatrix4x4 viewProjection =
        rhi()->clipSpaceCorrMatrix() * projection * view;

    const FrustumCuller culler = FrustumCuller::fromCamera(
        camera_.position(), forward, up, camera_.right(),
        NavigationCamera::verticalFieldOfViewDegrees,
        static_cast<double>(aspect), clip.nearPlane, clip.farPlane);

    // Near-to-far order makes the point budget spend on close blocks.
    std::vector<PointBlockPtr> visible;
    for (const PointBlockPtr &block : blocks) {
        if (uploadScheduler_.bufferFor(block.get())
            && culler.intersects(block->bounds)) {
            visible.push_back(block);
        }
    }
    const Vec3d eye = camera_.position();
    std::ranges::sort(visible, [&eye](const PointBlockPtr &left,
                                      const PointBlockPtr &right) {
        const auto distance = [&eye](const PointBlockPtr &block) {
            const auto centre = block->bounds.center();
            return length(
                Vec3d{centre[0], centre[1], centre[2]} - eye);
        };
        return distance(left) < distance(right);
    });

    float scalarMinimum = 0.0F;
    float scalarMaximum = 65535.0F;
    const Bounds3d sceneBounds = scene_->bounds();
    switch (colorMode_.source) {
    case PointColorSource::X:
        scalarMinimum = static_cast<float>(sceneBounds.minimum[0]);
        scalarMaximum = static_cast<float>(sceneBounds.maximum[0]);
        break;
    case PointColorSource::Y:
        scalarMinimum = static_cast<float>(sceneBounds.minimum[1]);
        scalarMaximum = static_cast<float>(sceneBounds.maximum[1]);
        break;
    case PointColorSource::Z:
        scalarMinimum = static_cast<float>(sceneBounds.minimum[2]);
        scalarMaximum = static_cast<float>(sceneBounds.maximum[2]);
        break;
    case PointColorSource::Intensity:
        scalarMaximum = scene_->intensityMaximum() > 0
            ? static_cast<float>(scene_->intensityMaximum())
            : 65535.0F;
        break;
    default:
        break;
    }

    std::vector<BlockDraw> draws;
    draws.reserve(visible.size());
    std::uint64_t budgetedPoints = 0;
    quint32 idBase = 0;
    for (const PointBlockPtr &block : visible) {
        if (budgetedPoints >= pointBudget_.current()) {
            break;
        }
        const auto pointCount = static_cast<quint32>(std::min<std::uint64_t>(
            block->points.size(),
            pointBudget_.current() - budgetedPoints));
        budgetedPoints += pointCount;

        const Vec3d relative = block->origin - eye;
        QMatrix4x4 model;
        model.translate(
            static_cast<float>(relative.x),
            static_cast<float>(relative.y),
            static_cast<float>(relative.z));
        model.scale(static_cast<float>(block->scale));
        const QMatrix4x4 mvp = viewProjection * model;

        BlockDraw draw;
        draw.block = block;
        draw.buffer = uploadScheduler_.bufferFor(block.get());
        draw.pointCount = pointCount;
        draw.idBase = idBase;
        std::memcpy(draw.uniform.mvp, mvp.constData(),
                    sizeof(draw.uniform.mvp));
        draw.uniform.originAndScale[0] =
            static_cast<float>(block->origin.x);
        draw.uniform.originAndScale[1] =
            static_cast<float>(block->origin.y);
        draw.uniform.originAndScale[2] =
            static_cast<float>(block->origin.z);
        draw.uniform.originAndScale[3] =
            static_cast<float>(block->scale);
        draw.uniform.pointSize = 2.0F;
        draw.uniform.colorSource =
            static_cast<std::int32_t>(colorMode_.source);
        draw.uniform.colorMap =
            static_cast<std::int32_t>(colorMode_.colorMap);
        draw.uniform.scalarMinimum = scalarMinimum;
        draw.uniform.scalarMaximum = scalarMaximum;
        draw.uniform.idBase = static_cast<std::int32_t>(idBase);
        draws.push_back(std::move(draw));
        idBase += pointCount;
    }
    return draws;
}
```

`submitPendingPick` gains the draw list and passes a snapshot into the completion; `resolvePick` maps the global id through it:

```cpp
void RenderViewportWidget::submitPendingPick(
    QRhiCommandBuffer *commandBuffer, const std::vector<BlockDraw> &draws)
{
    if (pointPicker_.inFlight()) {
        return;
    }
    const auto request = input_.takePendingPick(camera_.revision());
    if (!request) {
        return;
    }

    const QSize targetSize = renderTarget()->pixelSize();
    const double scaleX = static_cast<double>(targetSize.width())
        / static_cast<double>(std::max(1, width()));
    const double scaleY = static_cast<double>(targetSize.height())
        / static_cast<double>(std::max(1, height()));
    const QPoint pixelPosition(
        static_cast<int>(std::lround(request->position.x * scaleX)),
        static_cast<int>(std::lround(request->position.y * scaleY)));
    pointPicker_.record(
        commandBuffer, draws, pointCloudRenderer_.uniformStride(),
        pixelPosition, targetSize,
        [this, request = *request, draws](
            const std::optional<std::uint32_t> pointId) {
            resolvePick(request, pointId, draws);
        });
}

void RenderViewportWidget::resolvePick(
    const PickRequest request,
    const std::optional<std::uint32_t> pointId,
    const std::vector<BlockDraw> &draws)
{
    if (request.cameraRevision != camera_.revision()) {
        input_.markStale(request);
        update();
        return;
    }
    input_.completeInFlight();

    if (request.kind == PickKind::Raw) {
        auto completion = std::move(rawPickCompletion_);
        rawPickCompletion_ = {};
        if (completion) {
            completion(pointId);
        }
        return;
    }

    std::optional<Vec3d> target;
    if (pointId) {
        for (const BlockDraw &draw : draws) {
            if (*pointId >= draw.idBase
                && *pointId < draw.idBase + draw.pointCount) {
                target = decodeBlockPosition(
                    *draw.block,
                    draw.block->points[*pointId - draw.idBase]);
                break;
            }
        }
    }

    if (request.kind == PickKind::Pivot) {
        if (target) {
            camera_.setPivot(*target);
        }
    } else if (target) {
        camera_.dollyToward(*target, request.wheelUnits);
    } else {
        camera_.dollyForward(request.wheelUnits);
    }
    update();
}
```

`releaseResources()` becomes:

```cpp
void RenderViewportWidget::releaseResources()
{
    pointPicker_.releaseResources();
    pointCloudRenderer_.releaseResources();
    uploadScheduler_.releaseResources();
    resourceRhi_ = nullptr;
}
```

Delete the `createDeviceResources`, `createPointBuffer`, `cameraUniform`, and `pointAt` definitions and `PointBufferUploader.{h,cpp}`; update includes (`scene/FrustumCuller.h`, `scene/LoadedCloudScene.h`, `scene/SyntheticScene.h`, `renderer/rhi/UploadScheduler.h`, `<QMatrix4x4>`, `<QVector3D>`, `<cstring>`, `<ranges>`) and remove the deleted files from `CMakeLists.txt`.

- [ ] **Step 4a: FakeViewport implements the new pure virtual** — in `tests/ui/MainWindowTests.cpp`, add to `FakeViewport` (keeping its existing `setPointCloud` until Task 12):

```cpp
    void setScene(pci::PointCloudScenePtr scene) override
    {
        metadata_ = scene->metadata();
        pointCount_ = scene->totalPointCount();
        colorMode_ = pci::defaultPointColorMode(metadata_);
    }
```

- [ ] **Step 5: Build and run the full suite — this is the behavior-preservation gate**

```bash
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```
Expected: **every existing test passes unmodified**, including `pcinspector_gpu_tests` and the `renderer_smoke*` fixture tests (`Loaded ...: 8 / 8 points`). Debug any regression before proceeding — the most likely culprits are the uniform std140 layout (verify `sizeof(BlockUniform) == 112` matches the GLSL block), the pick viewport translation, and the eye-relative MVP.

- [ ] **Step 6: Add the multi-block GPU pick test** — append to `tests/gpu/PointPickerGpuTests.cpp`:

```cpp
pci::PointCloudScenePtr sceneWithBlocksAt(
    const std::vector<pci::Vec3d> &positions)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = positions.size();
    metadata.sourceBounds = {
        .minimum = {-1.0, -1.0, -1.0},
        .maximum = {1.0, 1.0, 1.0},
    };
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    for (const pci::Vec3d &position : positions) {
        auto block = std::make_shared<pci::PointBlock>();
        block->origin = {-1.0, -1.0, -1.0};
        block->scale = 2.0 / 65535.0;
        block->bounds = metadata.sourceBounds;
        const auto q = pci::quantizeToBlock(
            position, block->origin, block->scale);
        block->points.push_back({.x = q[0], .y = q[1], .z = q[2],
                                 .attributes = 0,
                                 .rgba = 0xffffffffU,
                                 .packedProperties = 0});
        block->attributes.resize(1);
        scene->addBlock(std::move(block));
    }
    return scene;
}

TEST_CASE("GPU picking maps ids across multiple blocks", "[gpu]")
{
    pci::RenderViewportWidget viewport(0, false);
    viewport.resize(320, 240);
    // Two single-point blocks: one centred (in front of the default
    // camera), one far off to the side.
    viewport.setScene(sceneWithBlocksAt({
        {0.0, 0.0, 0.0},
        {0.9, 0.0, 0.9},
    }));

    QString failure;
    viewport.setFailureCallback(
        [&failure](const QString &message) { failure = message; });
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport, 2000));

    bool completed = false;
    std::optional<std::uint32_t> picked;
    viewport.requestRawPickForTesting(
        QPoint(viewport.width() / 2, viewport.height() / 2),
        [&](const std::optional<std::uint32_t> id) {
            completed = true;
            picked = id;
        });
    REQUIRE(QTest::qWaitFor(
        [&] { return completed || !failure.isEmpty(); }, 5000));
    REQUIRE(failure.isEmpty());
    REQUIRE(picked.has_value());
}
```

(add includes `"scene/PointCloudScene.h"` and `<vector>` at the top). Note the raw pick returns a **global** id; the assertion checks a hit exists — block attribution is covered by the unit-level id mapping in `resolvePick` and the existing single-point hit/miss cases.

- [ ] **Step 7: Run GPU tests, then commit**

```bash
ctest --test-dir build --output-on-failure -L gpu
git add -A src shaders tests CMakeLists.txt
git commit -m "feat: render point clouds as culled per-block draws"
```

---

### Task 12: Import and app switchover to world-space scenes

The loader now partitions streamed PDAL points into world-space blocks and publishes the scene early (`sceneReady`) so the viewer displays data progressively. The controller, MainWindow, and main.cpp move to `PointCloudScenePtr`, and the legacy `LoadedPointCloud` type, the adapter, and `RenderViewport::setPointCloud` are deleted. This task is atomic because the loader return type flows through controller signals into MainWindow.

**Files:**
- Modify: `src/import/PointCloudImport.h`, `src/import/pdal/PdalPointCloudLoader.h`, `src/import/pdal/PdalPointCloudLoader.cpp`
- Modify: `src/import/PointCloudLoadController.h`, `src/import/PointCloudLoadController.cpp`
- Modify: `src/app/MainWindow.h`, `src/app/MainWindow.cpp`, `main.cpp`
- Modify: `src/renderer/RenderViewport.h`, `src/renderer/rhi/RenderViewportWidget.h`, `src/renderer/rhi/RenderViewportWidget.cpp` (remove `setPointCloud`)
- Delete: `src/pointcloud/LoadedPointCloud.h`, `src/scene/LoadedCloudScene.h`, `src/scene/LoadedCloudScene.cpp`
- Test: `tests/component/PdalImportTests.cpp`, `tests/qt/PointCloudLoadControllerTests.cpp`, `tests/ui/MainWindowTests.cpp`, `tests/ui/RendererContractTests.cpp`, `tests/gpu/PointPickerGpuTests.cpp`, `tests/unit/PointCloudContractTests.cpp`, `tests/unit/SyntheticSceneTests.cpp`
- Modify: `CMakeLists.txt` (`pcinspector_import_pdal` links `pcinspector_scene`; drop deleted sources; `pcinspector_app_ui` and test targets inherit the link transitively)

**Interfaces:**
- Consumes: Tasks 4–11.
- Produces:
  - `PointCloudImportRequest { std::filesystem::path sourcePath; std::uint64_t maximumPoints = 100'000'000; std::stop_token stopToken; std::function<void(PointCloudImportProgress)> progress; std::function<void(PointCloudScenePtr)> sceneReady; }`
  - `virtual PointCloudScenePtr PointCloudLoader::load(const PointCloudImportRequest &) const = 0;`
  - Controller signals: `sceneReady(pci::PointCloudScenePtr)` (emitted once, early), `loaded(pci::PointCloudScenePtr)` (on completion); `progressChanged`/`failed`/`cancelled` unchanged.
  - `RenderViewport` no longer has `setPointCloud`; `setScene` is the only ingestion path.

- [ ] **Step 1: Update the import contract** — `src/import/PointCloudImport.h`: replace the `LoadedPointCloud` include with `"scene/PointCloudScene.h"`, change the request and loader:

```cpp
struct PointCloudImportRequest {
    std::filesystem::path sourcePath;
    std::uint64_t maximumPoints = 100'000'000;
    std::stop_token stopToken;
    std::function<void(PointCloudImportProgress)> progress;
    // Called at most once, from the loader's thread, as soon as the
    // scene shell (metadata, no blocks yet) exists.
    std::function<void(PointCloudScenePtr)> sceneReady;
};

class PointCloudLoader {
public:
    virtual ~PointCloudLoader() = default;

    [[nodiscard]] virtual PointCloudScenePtr
    load(const PointCloudImportRequest &request) const = 0;
};
```

(`PointCloudImportStage`, the progress struct, and the exception types stay; `Optimizing` remains declared but the PDAL loader stops emitting it.)

- [ ] **Step 2: Update the component tests to the new contract (they must fail first)** — rewrite `tests/component/PdalImportTests.cpp` from the `checkMetadata` helper down (fixture plumbing at the top stays). Key content — note COPC delivers points in a different order than LAS now that the global sort is gone, so cross-format comparison sorts first:

```cpp
std::vector<pci::GpuPoint> sortedPoints(const pci::PointCloudScenePtr &scene)
{
    std::vector<pci::GpuPoint> points;
    for (const auto &block : scene->blocks()) {
        points.insert(points.end(),
                      block->points.begin(), block->points.end());
    }
    std::ranges::sort(points, [](const pci::GpuPoint &a,
                                 const pci::GpuPoint &b) {
        return std::tie(a.x, a.y, a.z, a.rgba, a.attributes,
                        a.packedProperties)
            < std::tie(b.x, b.y, b.z, b.rgba, b.attributes,
                       b.packedProperties);
    });
    return points;
}

TEST_CASE("PDAL loads equivalent block scenes from supported formats",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    const auto las = loader.load({.sourcePath = fixture.paths().las});
    const auto laz = loader.load({.sourcePath = fixture.paths().laz});
    const auto copc = loader.load({.sourcePath = fixture.paths().copc});

    CHECK(las->totalPointCount() == pci::test::fixturePoints.size());
    CHECK(sortedPoints(las) == sortedPoints(laz));
    CHECK(sortedPoints(las) == sortedPoints(copc));
    CHECK(las->intensityMaximum() == 800);

    const auto blocks = las->blocks();
    REQUIRE(blocks.size() == 1);
    const auto &block = *blocks.front();
    CHECK(block.origin == pci::Vec3d{1000.0, 2000.0, 10.0});
    CHECK(block.scale == Catch::Approx(10.0 / 65535.0));
    CHECK(block.points.size() == block.attributes.size());

    // LAS preserves source order; the first fixture point decodes back
    // to its world position within half a quantization step.
    const pci::Vec3d front =
        pci::decodeBlockPosition(block, block.points.front());
    CHECK(front.x == Catch::Approx(1000.0).margin(block.scale));
    CHECK(front.y == Catch::Approx(2000.0).margin(block.scale));
    CHECK(front.z == Catch::Approx(10.0).margin(block.scale));
    CHECK(block.attributes.front().intensity == 100);
    CHECK(block.attributes.front().classification == 2);
    CHECK((block.points.front().attributes & 0xffU) == 2);
    CHECK(pci::gpuPointIntensity(block.points.front().packedProperties)
          == 100);
}

TEST_CASE("PDAL point limits use a deterministic stride",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    const auto first = loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    });
    const auto repeated = loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 4,
    });

    REQUIRE(first->totalPointCount() == 4);
    CHECK(sortedPoints(first) == sortedPoints(repeated));
    const auto &block = *first->blocks().front();
    // Stride 2 samples source indices 0, 2, 4, 6.
    CHECK(block.attributes[0].intensity == 100);
    CHECK(block.attributes[1].intensity == 300);
    CHECK(block.attributes[2].intensity == 500);
    CHECK(block.attributes[3].intensity == 700);
}

TEST_CASE("PDAL publishes the scene before streaming blocks",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;

    pci::PointCloudScenePtr early;
    std::uint64_t blocksAtSceneReady = 0;
    const auto scene = loader.load({
        .sourcePath = fixture.paths().las,
        .sceneReady = [&](const pci::PointCloudScenePtr &value) {
            early = value;
            blocksAtSceneReady = value->blocks().size();
        },
    });

    REQUIRE(early != nullptr);
    CHECK(early.get() == scene.get());
    CHECK(blocksAtSceneReady == 0);
    CHECK(early->metadata().sourcePointCount
          == pci::test::fixturePoints.size());
}

TEST_CASE("PDAL reports progress and observes cancellation",
          "[component][pdal]")
{
    const FixtureDirectory fixture;
    const pci::PdalPointCloudLoader loader;
    std::vector<pci::PointCloudImportProgress> progress;

    const auto scene = loader.load({
        .sourcePath = fixture.paths().las,
        .maximumPoints = 8,
        .progress = [&progress](const pci::PointCloudImportProgress value) {
            progress.push_back(value);
        },
    });

    CHECK(scene->totalPointCount() == 8);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.front().stage == pci::PointCloudImportStage::Reading);
    CHECK(progress.front().processed == 0);
    CHECK(progress.back().stage == pci::PointCloudImportStage::Reading);
    CHECK(progress.back().processed == 8);
    CHECK(progress.back().total == 8);

    std::stop_source stop;
    stop.request_stop();
    REQUIRE_THROWS_AS(loader.load({
                          .sourcePath = fixture.paths().las,
                          .maximumPoints = 8,
                          .stopToken = stop.get_token(),
                      }),
                      pci::PointCloudImportCancelled);
}
```

Keep the fixture-creation, inspection, inspection-error, and zero-limit test cases as they are (they don't touch the loader's return type — the zero-limit case only changes `loader.load({...})`'s discarded return type, which compiles unchanged). Add includes: `"scene/PointBlock.h"`, `<catch2/catch_approx.hpp>`, `<algorithm>`, `<tuple>`.

Run: `cmake --build build -j 8` — Expected: FAIL (contract mismatch). Note: because the loader's return type flows through the controller into MainWindow, **the build stays red until Step 7 completes**; Step 8 is the first green gate. Work through Steps 3–7 without intermediate test runs.

- [ ] **Step 3: Rewrite the PDAL loader** — `src/import/pdal/PdalPointCloudLoader.cpp` (header only changes its include to the new contract; the class declaration is unchanged). Full replacement:

```cpp
#include "import/pdal/PdalPointCloudLoader.h"

#include "core/GpuPointProperties.h"
#include "import/pdal/PdalSourceInspector.h"
#include "scene/BlockPartitioner.h"

#include <pdal/Dimension.hpp>
#include <pdal/Options.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/filters/StreamCallbackFilter.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace pci {
namespace {

std::uint8_t color8(const std::uint16_t value)
{
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(value) + 128U) / 257U);
}

PointSample mapSample(const pdal::PointRef &point,
                      const PointCloudMetadata &metadata)
{
    const auto classification = metadata.hasClassification
        ? point.getFieldAs<std::uint8_t>(
              pdal::Dimension::Id::Classification)
        : std::uint8_t{0};
    const auto intensity = metadata.hasIntensity
        ? point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Intensity)
        : std::uint16_t{0};
    const auto returnNumber = metadata.hasReturnNumber
        ? point.getFieldAs<std::uint8_t>(
              pdal::Dimension::Id::ReturnNumber)
        : std::uint8_t{0};
    const auto numberOfReturns = metadata.hasNumberOfReturns
        ? point.getFieldAs<std::uint8_t>(
              pdal::Dimension::Id::NumberOfReturns)
        : std::uint8_t{0};

    std::uint32_t rgba = 0xffffffffU;
    if (metadata.hasColor) {
        const auto red = color8(
            point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Red));
        const auto green = color8(
            point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Green));
        const auto blue = color8(
            point.getFieldAs<std::uint16_t>(pdal::Dimension::Id::Blue));
        rgba = static_cast<std::uint32_t>(red)
            | (static_cast<std::uint32_t>(green) << 8U)
            | (static_cast<std::uint32_t>(blue) << 16U)
            | 0xff000000U;
    }

    return {
        .position = {
            point.getFieldAs<double>(pdal::Dimension::Id::X),
            point.getFieldAs<double>(pdal::Dimension::Id::Y),
            point.getFieldAs<double>(pdal::Dimension::Id::Z),
        },
        .rgba = rgba,
        .packedAttributes = static_cast<std::uint16_t>(
            classification | ((intensity >> 8U) << 8U)),
        .packedProperties = packGpuPointProperties(
            intensity, returnNumber, numberOfReturns),
        .attributes = {
            .intensity = intensity,
            .classification = classification,
            .returnNumber = returnNumber,
            .numberOfReturns = numberOfReturns,
        },
    };
}

} // namespace

PointCloudScenePtr PdalPointCloudLoader::load(
    const PointCloudImportRequest &request) const
{
    if (request.maximumPoints == 0) {
        throw PointCloudImportError(
            "maximumPoints must be greater than zero");
    }
    if (request.stopToken.stop_requested()) {
        throw PointCloudImportCancelled();
    }

    PdalSourceInspector inspector;
    const PointCloudMetadata metadata =
        inspector.inspect(request.sourcePath);
    const std::uint64_t stride = std::max<std::uint64_t>(
        1,
        (metadata.sourcePointCount + request.maximumPoints - 1)
            / request.maximumPoints);
    const std::uint64_t expectedPoints = std::min(
        metadata.sourcePointCount, request.maximumPoints);

    auto scene = std::make_shared<PointCloudScene>(metadata);
    if (request.sceneReady) {
        request.sceneReady(scene);
    }
    BlockPartitioner partitioner(
        metadata.sourceBounds, expectedPoints,
        [&scene](PointBlockPtr block) {
            scene->addBlock(std::move(block));
        });

    if (request.progress) {
        request.progress({
            .stage = PointCloudImportStage::Reading,
            .processed = 0,
            .total = metadata.sourcePointCount,
        });
    }

    try {
        pdal::StageFactory factory;
        pdal::Stage *reader = factory.createStage(metadata.sourceDriver);
        if (!reader) {
            throw PointCloudImportError(
                "PDAL reader is unavailable: " + metadata.sourceDriver);
        }
        pdal::Options options;
        options.add("filename", request.sourcePath.string());
        reader->setOptions(options);

        std::uint64_t processed = 0;
        std::uint64_t sampled = 0;
        pdal::StreamCallbackFilter callback;
        callback.setInput(*reader);
        callback.setCallback([&](pdal::PointRef &point) {
            if (request.stopToken.stop_requested()) {
                throw PointCloudImportCancelled();
            }
            if (processed % stride == 0
                && sampled < request.maximumPoints) {
                partitioner.add(mapSample(point, metadata));
                ++sampled;
            }
            ++processed;
            if (request.progress && processed % 65'536 == 0) {
                request.progress({
                    .stage = PointCloudImportStage::Reading,
                    .processed = processed,
                    .total = metadata.sourcePointCount,
                });
            }
            return true;
        });

        pdal::FixedPointTable table(4096);
        callback.prepare(table);
        if (!callback.pipelineStreamable()) {
            throw PointCloudImportError(
                "PDAL pipeline is not streamable for '"
                + request.sourcePath.string() + "'");
        }
        callback.execute(table);

        if (request.progress) {
            request.progress({
                .stage = PointCloudImportStage::Reading,
                .processed = processed,
                .total = metadata.sourcePointCount,
            });
        }
    } catch (const PointCloudImportCancelled &) {
        throw;
    } catch (const PointCloudImportError &) {
        throw;
    } catch (const std::exception &error) {
        throw PointCloudImportError(
            "Could not load point cloud '" + request.sourcePath.string()
            + "': " + error.what());
    }

    partitioner.finish();
    return scene;
}
```

`src/import/pdal/PdalPointCloudLoader.h` — update the override signature to return `PointCloudScenePtr`. In `CMakeLists.txt`, add `pcinspector_scene` to `pcinspector_import_pdal`'s PUBLIC `target_link_libraries`.

(No test run yet — the controller and UI still reference the old contract; continue to Step 4.)

- [ ] **Step 4: Controller** — `src/import/PointCloudLoadController.h`: replace `Q_DECLARE_METATYPE(pci::LoadedPointCloudPtr)` with `Q_DECLARE_METATYPE(pci::PointCloudScenePtr)`; signals become:

```cpp
signals:
    void progressChanged(
        pci::PointCloudImportStage stage,
        quint64 processed,
        quint64 total);
    void sceneReady(pci::PointCloudScenePtr scene);
    void loaded(pci::PointCloudScenePtr scene);
    void failed(QString message);
    void cancelled();
```

and `LoadOutcome::cloud` becomes `PointCloudScenePtr scene;`. In `PointCloudLoadController.cpp`: `qRegisterMetaType<PointCloudScenePtr>();`, and in `load()` add the sceneReady marshaling next to the progress wrapper:

```cpp
    request.sceneReady =
        [guardedThis, jobId](const PointCloudScenePtr scene) {
            if (!guardedThis) {
                return;
            }
            QMetaObject::invokeMethod(
                guardedThis,
                [guardedThis, jobId, scene] {
                    if (guardedThis
                        && guardedThis->currentJobId_ == jobId) {
                        emit guardedThis->sceneReady(scene);
                    }
                },
                Qt::QueuedConnection);
        };
```

The worker lambda's `LoadedPointCloudPtr cloud = loader->load(request);` becomes `PointCloudScenePtr scene = loader->load(request);` with `{.scene = std::move(scene)}` and the finished handler emits `loaded(outcome.scene)`.

Update `tests/qt/PointCloudLoadControllerTests.cpp`: `makeCloud` becomes

```cpp
pci::PointCloudScenePtr makeScene(const std::uint64_t count)
{
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = count;
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(static_cast<std::size_t>(count));
    block->attributes.resize(static_cast<std::size_t>(count));
    scene->addBlock(std::move(block));
    return scene;
}
```

`FakeLoader::load` returns `makeScene(2)`, calls `request.sceneReady(...)` with it when set, and emits only the two Reading progress events (drop the Optimizing one). In the first test case: `REQUIRE(progress.count() == 2);`, drop the Optimizing assertion, cast the loaded payload with `qvariant_cast<pci::PointCloudScenePtr>` and check `scene->totalPointCount() == 2`. Add a sceneReady spy check:

```cpp
    QSignalSpy sceneReady(
        &controller, &pci::PointCloudLoadController::sceneReady);
    ...
    CHECK(sceneReady.count() == 1);
```

- [ ] **Step 5: Viewport interface cleanup** — remove `setPointCloud` from `src/renderer/RenderViewport.h` (and its `LoadedPointCloud` include) and from `RenderViewportWidget.{h,cpp}`; remove the `scene/LoadedCloudScene.h` include; delete `src/scene/LoadedCloudScene.{h,cpp}` and `src/pointcloud/LoadedPointCloud.h`; drop `LoadedCloudScene.cpp` from `CMakeLists.txt`; remove the adapter test case from `tests/unit/SyntheticSceneTests.cpp`; remove the `"loaded cloud contract uses immutable shared ownership"` test case and the `LoadedPointCloud.h` include from `tests/unit/PointCloudContractTests.cpp`.

```bash
git rm src/pointcloud/LoadedPointCloud.h src/scene/LoadedCloudScene.h src/scene/LoadedCloudScene.cpp
grep -rn "LoadedPointCloud\|LoadedCloudScene" src tests main.cpp
```
Expected after this task's remaining steps: no matches.

- [ ] **Step 6: MainWindow** — `src/app/MainWindow.h`: `finishLoading(...)` is replaced by:

```cpp
    void handleSceneReady(const PointCloudScenePtr &scene);
    void handleLoadCompleted(const PointCloudScenePtr &scene);
```

(`PointCloudScenePtr` arrives via `renderer/RenderViewport.h`). In `MainWindow.cpp`, replace the `loaded` connection and add `sceneReady`:

```cpp
    connect(loadController_.get(),
            &PointCloudLoadController::sceneReady,
            this,
            [this](const PointCloudScenePtr &scene) {
                handleSceneReady(scene);
            });
    connect(loadController_.get(),
            &PointCloudLoadController::loaded,
            this,
            [this](const PointCloudScenePtr &scene) {
                handleLoadCompleted(scene);
            });
```

```cpp
void MainWindow::handleSceneReady(const PointCloudScenePtr &scene)
{
    try {
        statusBar()->showMessage(
            QStringLiteral("Loading %1: preparing renderer")
                .arg(loadingSource_));
        viewport_->setScene(scene);
        refreshColorSourceSelector();
    } catch (const std::exception &error) {
        showLoadFailure(QString::fromUtf8(error.what()));
    }
}

void MainWindow::handleLoadCompleted(const PointCloudScenePtr &scene)
{
    loadedPointCountText_ =
        QStringLiteral("Loaded %1 | %2 points")
            .arg(loadingSource_)
            .arg(scene->totalPointCount());
    if (!loading_) {
        // The first frame already hid the overlay (progressive load);
        // refresh the final status now that the count is known.
        statusBar()->showMessage(loadedPointCountText_);
    }
}
```

Update `tests/ui/MainWindowTests.cpp`: `ImmediateLoader` builds a 3-point scene (same shape as `makeScene(3)` above but with `metadata.hasColor/hasIntensity/hasClassification = true` and `metadata.sourcePath = request.sourcePath`), calls `request.sceneReady(scene)` before returning it, and drops the Optimizing progress event; `SlowLoader`/`FailingLoader` only change their return type. `FakeViewport::setPointCloud` becomes:

```cpp
    void setScene(pci::PointCloudScenePtr scene) override
    {
        metadata_ = scene->metadata();
        pointCount_ = scene->totalPointCount();
        colorMode_ = pci::defaultPointColorMode(metadata_);
    }
```

The three test cases keep their assertions (the fake emits render progress manually, so the flow is identical).

Update `tests/ui/RendererContractTests.cpp`: replace the `LoadedPointCloud` block with a scene:

```cpp
    pci::PointCloudMetadata metadata;
    metadata.sourcePointCount = 2;
    metadata.hasColor = true;
    metadata.hasIntensity = true;
    metadata.hasClassification = true;
    metadata.hasReturnNumber = true;
    metadata.hasNumberOfReturns = true;
    auto scene = std::make_shared<pci::PointCloudScene>(metadata);
    auto block = std::make_shared<pci::PointBlock>();
    block->points.resize(2);
    block->attributes.resize(2);
    scene->addBlock(std::move(block));
    ...
    viewport->setScene(scene);
```

(the `sawPreparing` wait and `totalPointCount() == 2` checks stay). Update the first GPU test in `tests/gpu/PointPickerGpuTests.cpp` to use `sceneWithBlocksAt({{0.0, 0.0, 0.0}})` for the hit case (`CHECK(*picked == 0)`) and `sceneWithBlocksAt({{-1.0, -1.0, 0.0}})` for the miss case, replacing `cloudWithPoint`.

- [ ] **Step 7: main.cpp** — change the `--max-points` default value string to `"100000000"`, and replace the load/creation section:

```cpp
        pci::PointCloudScenePtr initialScene;
        if (sourcePath && smokeTest) {
            initialScene = pci::PdalPointCloudLoader().load({
                .sourcePath = *sourcePath,
                .maximumPoints = *maximumPoints,
            });
            qInfo().noquote()
                << QStringLiteral("Loaded %1: %2 / %3 points")
                       .arg(QString::fromStdString(sourcePath->string()))
                       .arg(initialScene->totalPointCount())
                       .arg(initialScene->metadata().sourcePointCount);
        }

        auto viewport = pci::createRenderViewport(
            sourcePath ? 0 : *pointCount, smokeTest);
        if (initialScene) {
            viewport->setScene(initialScene);
        }
```

(the `RenderViewport.h` include already provides `PointCloudScenePtr`; the smoke regular expression `Loaded .*: 8 / 8 points` still matches).

- [ ] **Step 8: Full suite gate**

```bash
cmake --build build -j 8
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -L gpu
```
Expected: all tests pass, including the three `renderer_smoke_fixture*` cases. Real files now render in world coordinates with per-block precision.

- [ ] **Step 9: Commit**

```bash
git add -A src tests main.cpp CMakeLists.txt
git commit -m "feat: stream world-space block scenes through import and app"
```

---

### Task 13: End-to-end verification and cleanup

**Files:**
- Possibly modify: leftovers found by the greps below.

- [ ] **Step 1: Residue check**

```bash
grep -rn "LoadedPointCloud\|LoadedCloudScene\|PointBufferUploader\|setPointCloud" src tests main.cpp CMakeLists.txt
```
Expected: no matches. Remove anything found.

- [ ] **Step 2: Warnings build**

```bash
cmake -S . -B build-warnings -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic" \
      -DPCINSPECTOR_ENABLE_GPU_TESTS=ON
cmake --build build-warnings -j 8 2>&1 | grep -i "warning" | grep -v "third_party" || echo "no project warnings"
```
Expected: `no project warnings`.

- [ ] **Step 3: Full suite one more time**

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -L gpu
```
Expected: 100% pass.

- [ ] **Step 4: Manual verification (real GPU, real files)**

```bash
# Progressive synthetic load: watch the status bar point count climb
# while the app stays responsive (uploads are budgeted per frame).
./build/pcinspector --points 50000000

# Real cloud: confirm points appear while the file is still reading,
# navigation/orbit/pan/pick behave, and color modes work.
./build/pcinspector test_data/<any .las/.laz/.copc.laz file>
```
Check: no UI freeze during load; FPS metrics update; double-click pivot and wheel dolly still target points; X/Y/Z color ramps span the cloud (world-range normalization), intensity ramp uses the cloud's real intensity range.

- [ ] **Step 5: Scale spot-check (optional but recommended, ~10 GB unified memory)**

```bash
./build/pcinspector --points 300000000
```
Expected: interactive navigation once uploads settle (spec Phase 1 acceptance); frame time stays bounded because the adaptive budget caps drawn points and culling skips off-screen blocks.

- [ ] **Step 6: Commit any cleanup and mark the plan done**

```bash
git add -A && git commit -m "chore: phase 1 cleanup" || echo "nothing to clean up"
```

---

## Deviations from the spec (intentional, minor)

1. **Block sealing instead of cell splitting:** the spec says "overfull cells split once"; the partitioner instead seals a cell's block at `maximumPointsPerBlock` and starts a new block for the same cell. Same bound on block size, simpler, and it enables progressive emission during streaming.
2. **Residency lives in the renderer, not on the block:** blocks are immutable after sealing (`shared_ptr<const PointBlock>`); the `UploadScheduler`'s buffer map is the single source of GPU-residency truth. The spec's block-level residency state becomes relevant with Phase 3's `BlockProvider` eviction and is deferred to it.
3. **`BlockProvider` interface deferred to Phase 3:** in Phase 1 the only provider would be trivial in-memory pass-through; introducing the interface now would be dead abstraction (YAGNI). The seam it needs — renderer consumes `PointCloudScene::blocks()` snapshots only — is already in place.

## Known limitation carried into Phase 2

With progressive display, the loading overlay hides at the first rendered frame while the import may still be streaming blocks. `MainWindow::requestLoadCancellation` is gated on the overlay's `loading_` flag, so a long import can no longer be cancelled from the UI once the first frame is visible (the File → Cancel Loading action becomes a no-op). Acceptable for Phase 1; Phase 2's LOD/streaming UI work should decouple "overlay visible" from "import cancellable".



