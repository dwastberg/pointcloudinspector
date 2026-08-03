# QVulkanWindow Renderer Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Qt Widgets proof of concept that renders and benchmarks ten million deterministic quantized points through `QVulkanWindow`.

**Architecture:** Qt owns the Vulkan instance integration, device, queue, swapchain, default render pass, and primary frame command buffers. Focused renderer code owns buffers, descriptors, shaders, timestamp queries, camera uniforms, and draw recording; pure C++ modules own point generation, option parsing, orbit state, and point-budget policy.

**Tech Stack:** C++23, CMake, Qt 6 Widgets/Gui, Vulkan 1.1, GLSL 450 compiled with `glslc`, CTest.

**Repository note:** This directory has no Git metadata. Execute in place and omit commit steps.

---

## File Map

- `CMakeLists.txt`: targets, shader compilation, resources, and CTest wiring.
- `main.cpp`: application options, Vulkan instance creation, and window startup.
- `src/core/AppOptions.h/.cpp`: strict `--points` parsing.
- `src/core/GpuPoint.h`: shared 16-byte point layout.
- `src/core/SyntheticPointCloud.h/.cpp`: deterministic chunk generation.
- `src/core/AdaptivePointBudget.h/.cpp`: frame-time-based draw budget.
- `src/core/OrbitCamera.h/.cpp`: bounded yaw, pitch, and distance state.
- `src/app/MainWindow.h/.cpp`: Qt shell, native viewport container, and metrics.
- `src/renderer/RenderMetrics.h`: renderer-to-shell metric snapshot.
- `src/renderer/RenderViewport.h/.cpp`: `QVulkanWindow`, input, camera matrix, and renderer factory.
- `src/renderer/VulkanPointRenderer.h/.cpp`: Vulkan resources and frame recording.
- `shaders/points.vert`: quantized position reconstruction and point sizing.
- `shaders/points.frag`: point color output.
- `tests/core_tests.cpp`: dependency-free tests for all core modules.

## Task 0: Install and Verify Vulkan Build Prerequisites

**Files:**
- No repository files.

- [ ] **Step 1: Install the missing local dependencies**

Run:

```bash
brew install vulkan-headers vulkan-loader molten-vk shaderc
```

Expected: Homebrew installs Vulkan headers, the loader, the MoltenVK
implementation, and `glslc`.

- [ ] **Step 2: Verify discovery**

Run:

```bash
test -f /opt/homebrew/include/vulkan/vulkan.h
test -x /opt/homebrew/bin/glslc
find /opt/homebrew -name 'libvulkan*.dylib' -o -name 'libMoltenVK*.dylib'
```

Expected: both tests exit zero and the search prints Vulkan loader and MoltenVK
libraries.

## Task 1: Strict Point-Count Options

**Files:**
- Create: `tests/core_tests.cpp`
- Create: `src/core/AppOptions.h`
- Create: `src/core/AppOptions.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing option tests**

Create a small exception-based test runner and assertions equivalent to:

```cpp
using pci::parsePointCount;
CHECK(parsePointCount("10000000") == 10'000'000ULL);
CHECK(parsePointCount("1") == 1ULL);
CHECK(!parsePointCount("0"));
CHECK(!parsePointCount("-1"));
CHECK(!parsePointCount("12x"));
CHECK(!parsePointCount("18446744073709551616"));
```

Add a temporary `pcinspector_core_tests` CMake target containing only
`tests/core_tests.cpp` so the missing production header is observed by the
compiler.

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target pcinspector_core_tests
```

Expected: compilation fails because `src/core/AppOptions.h` does not exist.

- [ ] **Step 3: Implement the parser and core target**

Expose:

```cpp
namespace pci {
std::optional<std::uint64_t> parsePointCount(std::string_view text);
}
```

Use `std::from_chars`, require complete input consumption, and reject zero.
Correct the existing `CMAKE_/AUTOMOC` typo, set C++23 without compiler
extensions, create `pcinspector_core`, create `pcinspector_core_tests`, and
register it with CTest.

- [ ] **Step 4: Verify GREEN**

Run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target pcinspector_core_tests
ctest --test-dir build --output-on-failure
```

Expected: one test executable passes.

## Task 2: Deterministic Quantized Point Chunks

**Files:**
- Create: `src/core/GpuPoint.h`
- Create: `src/core/SyntheticPointCloud.h`
- Create: `src/core/SyntheticPointCloud.cpp`
- Modify: `tests/core_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing layout and generation tests**

Add checks equivalent to:

```cpp
static_assert(sizeof(pci::GpuPoint) == 16);
const auto a = pci::generatePointChunk(0, 4096);
const auto b = pci::generatePointChunk(0, 4096);
CHECK(a == b);
CHECK(a.size() == 4096);
CHECK(pci::generatePointChunk(4096, 128) !=
      pci::generatePointChunk(0, 128));

std::array<bool, 8> octants{};
for (const auto &point : a) {
    const auto index = (point.x >= 32768 ? 1 : 0)
                     | (point.y >= 32768 ? 2 : 0)
                     | (point.z >= 32768 ? 4 : 0);
    octants[index] = true;
}
CHECK(std::ranges::all_of(octants, std::identity{}));
```

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build --target pcinspector_core_tests`.

Expected: compilation fails because `GpuPoint` and `generatePointChunk` are
undefined.

- [ ] **Step 3: Implement the point type and generator**

Define:

```cpp
struct GpuPoint {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t z;
    std::uint16_t attributes;
    std::uint32_t rgba;
    std::uint32_t padding;
    bool operator==(const GpuPoint &) const = default;
};

std::vector<GpuPoint> generatePointChunk(std::uint64_t firstIndex,
                                         std::size_t count);
```

Derive three independent 32-bit hashes from the global index, use their upper
16 bits for position, and derive RGB bytes from position. Set alpha to 255 and
all currently unused fields to zero.

- [ ] **Step 4: Verify GREEN**

Run `cmake --build build --target pcinspector_core_tests && ctest --test-dir build --output-on-failure`.

Expected: all core tests pass.

## Task 3: Adaptive Point Budget

**Files:**
- Create: `src/core/AdaptivePointBudget.h`
- Create: `src/core/AdaptivePointBudget.cpp`
- Modify: `tests/core_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing policy tests**

Add independent checks for this public API:

```cpp
pci::AdaptivePointBudget budget(10'000'000);
CHECK(budget.current() == 1'000'000);
budget.update(std::chrono::duration<double, std::milli>(20.0));
CHECK(budget.current() == 900'000);
budget.update(std::chrono::duration<double, std::milli>(10.0));
CHECK(budget.current() == 990'000);

pci::AdaptivePointBudget steady(10'000'000);
steady.update(std::chrono::duration<double, std::milli>(16.67));
CHECK(steady.current() == 1'000'000);

pci::AdaptivePointBudget tiny(50'000);
CHECK(tiny.current() == 50'000);
```

Repeated updates must also prove the 100,000 lower clamp and total-point upper
clamp.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build --target pcinspector_core_tests`.

Expected: compilation fails because `AdaptivePointBudget` does not exist.

- [ ] **Step 3: Implement the bounded controller**

Implement:

```cpp
class AdaptivePointBudget {
public:
    explicit AdaptivePointBudget(std::uint64_t totalPoints);
    void update(std::chrono::duration<double, std::milli> frameTime);
    [[nodiscard]] std::uint64_t current() const noexcept;
    [[nodiscard]] std::uint64_t total() const noexcept;
};
```

Decrease by exactly 10% above 18.34 ms, increase by exactly 10% below 13.34 ms,
leave the value unchanged inside the dead band, and clamp after every update.

- [ ] **Step 4: Verify GREEN**

Run `cmake --build build --target pcinspector_core_tests && ctest --test-dir build --output-on-failure`.

Expected: all core tests pass.

## Task 4: Orbit Camera State

**Files:**
- Create: `src/core/OrbitCamera.h`
- Create: `src/core/OrbitCamera.cpp`
- Modify: `tests/core_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing interaction-state tests**

Test that:

```cpp
pci::OrbitCamera camera;
camera.orbit(10.0F, 1000.0F);
CHECK(camera.yawDegrees() == 10.0F);
CHECK(camera.pitchDegrees() == 89.0F);
camera.zoom(1000.0F);
CHECK(camera.distance() == pci::OrbitCamera::minimumDistance);
camera.zoom(-1000.0F);
CHECK(camera.distance() == pci::OrbitCamera::maximumDistance);
```

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build --target pcinspector_core_tests`.

Expected: compilation fails because `OrbitCamera` does not exist.

- [ ] **Step 3: Implement bounded orbit and zoom**

Store yaw, pitch, and distance as floats. Orbit adds deltas and clamps pitch to
`[-89, 89]`. Zoom changes distance exponentially and clamps it to the public
minimum and maximum constants.

- [ ] **Step 4: Verify GREEN**

Run `cmake --build build --target pcinspector_core_tests && ctest --test-dir build --output-on-failure`.

Expected: all core tests pass.

## Task 5: Qt Shell, Viewport, Shaders, and Smoke-Test Contract

**Files:**
- Create: `src/app/MainWindow.h`
- Create: `src/app/MainWindow.cpp`
- Create: `src/renderer/RenderMetrics.h`
- Create: `src/renderer/RenderViewport.h`
- Create: `src/renderer/RenderViewport.cpp`
- Create: `shaders/points.vert`
- Create: `shaders/points.frag`
- Modify: `main.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a failing smoke-test contract**

Add `--smoke-test`, which requires the eventual renderer to close successfully
after presenting three frames. Register:

```cmake
add_test(NAME renderer_smoke
         COMMAND pcinspector --smoke-test --points 1000)
set_tests_properties(renderer_smoke PROPERTIES TIMEOUT 20)
```

Build and run `ctest --test-dir build -R renderer_smoke --output-on-failure`.

Expected: failure because the renderer executable and viewport behavior are not
implemented.

- [ ] **Step 2: Define shader behavior**

The vertex shader uses:

```glsl
#version 450
layout(location = 0) in uvec4 positionAttributes;
layout(location = 1) in vec4 color;
layout(binding = 0) uniform Camera {
    mat4 mvp;
    float pointSize;
} camera;
layout(location = 0) out vec4 pointColor;
void main() {
    vec3 position = vec3(positionAttributes.xyz) / 65535.0 * 2.0 - 1.0;
    gl_Position = camera.mvp * vec4(position, 1.0);
    gl_PointSize = camera.pointSize;
    pointColor = color;
}
```

The fragment shader forwards `pointColor` to a single color output.

- [ ] **Step 3: Implement the shell and viewport input boundary**

`MainWindow` embeds `RenderViewport` with `createWindowContainer()` and shows
formatted `RenderMetrics` in the status bar. `RenderViewport`:

- derives from `QVulkanWindow`;
- stores the requested point count and smoke-test flag;
- creates the renderer in `createRenderer()`;
- maps left-button drag to `OrbitCamera::orbit`;
- maps wheel delta to `OrbitCamera::zoom`; and
- returns a corrected perspective/view matrix for the renderer.

`main.cpp` parses `--points`, creates one `QVulkanInstance`, requests validation
in debug builds, reports instance failure with `QMessageBox`, associates the
instance with the viewport, and keeps the instance alive longer than the
window.

- [ ] **Step 4: Wire Vulkan and generated shaders in CMake**

Use `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)`,
`find_package(Vulkan REQUIRED)`, and `find_program(GLSLC glslc REQUIRED)`.
Compile both shaders with `glslc`, set stable Qt resource aliases, embed the
SPIR-V files under `:/shaders`, and link `Qt6::Core`, `Qt6::Gui`,
`Qt6::Widgets`, and `Vulkan::Vulkan`.

- [ ] **Step 5: Verify the CPU tests still pass**

Run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target pcinspector_core_tests
ctest --test-dir build -R pcinspector_core --output-on-failure
```

Expected: all core tests pass. The smoke test remains red until Task 6.

## Task 6: Vulkan Point Renderer

**Files:**
- Create: `src/renderer/VulkanPointRenderer.h`
- Create: `src/renderer/VulkanPointRenderer.cpp`
- Modify: `src/renderer/RenderViewport.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Implement checked Vulkan resource helpers**

Add focused private helpers for:

- loading aligned SPIR-V from `QFile`;
- selecting a compatible memory type from required property flags;
- creating and destroying a buffer plus its allocation;
- creating shader modules;
- submitting one staging copy through `graphicsCommandPool()` and
  `graphicsQueue()`; and
- formatting `VkResult` failures as `std::runtime_error`.

All partially created handles remain `VK_NULL_HANDLE` and cleanup is valid
after any exception.

- [ ] **Step 2: Create persistent renderer resources**

In `initResources()`:

1. obtain `QVulkanDeviceFunctions`;
2. allocate the full device-local point buffer;
3. allocate a reusable staging buffer for at most 1,000,000 points;
4. generate, upload, and discard one CPU chunk at a time;
5. create one coherent uniform buffer per concurrent frame;
6. create descriptor layout, pool, and one descriptor set per frame;
7. create two timestamp queries per concurrent frame when supported; and
8. cache the physical-device name and timestamp period.

Destroy these resources in strict reverse order in `releaseResources()`.

- [ ] **Step 3: Create swapchain-compatible pipeline state**

In `initSwapChainResources()`, create shader modules and a graphics pipeline
compatible with `defaultRenderPass()`. Configure:

- one 16-byte vertex binding;
- `R16G16B16A16_UINT` at offset 0;
- `R8G8B8A8_UNORM` at offset 8;
- point-list topology;
- dynamic viewport and scissor;
- depth testing with writes enabled;
- no blending or culling; and
- the sample count returned by `sampleCountFlagBits()`.

Destroy the pipeline and layout in `releaseSwapChainResources()`.

- [ ] **Step 4: Record continuous frames**

In `startNextFrame()`:

1. read the previous timestamp result for the current frame slot without
   blocking;
2. update the adaptive budget and smoothed metrics;
3. write the current camera matrix into that slot's uniform buffer;
4. reset and start timestamp queries;
5. begin the default render pass with color and depth clear values;
6. set viewport and scissor from `swapChainImageSize()`;
7. bind pipeline, descriptor set, and point buffer;
8. call `vkCmdDraw` with the active point budget;
9. end the render pass and timestamp;
10. call `frameReady()` and `requestUpdate()`; and
11. publish metrics at no more than 4 Hz.

If timestamp queries are unavailable, use `std::chrono::steady_clock` frame
durations and report `CPU` as the timing source.

- [ ] **Step 5: Make the smoke test observable**

Count completed `startNextFrame()` callbacks. In smoke-test mode, call
`QCoreApplication::quit()` after the third frame. Any initialization exception
must print a direct error and call `QCoreApplication::exit(1)`.

- [ ] **Step 6: Verify GREEN**

Run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: core tests and the Vulkan smoke test pass.

## Task 7: Runtime Validation and Benchmark Baseline

**Files:**
- Modify only files implicated by a reproduced failure.

- [ ] **Step 1: Run a debug validation smoke**

Run:

```bash
QT_LOGGING_RULES="qt.vulkan=true" ./build/pcinspector --smoke-test --points 100000
```

Expected: exit code zero, at least three frames, and no validation errors.

- [ ] **Step 2: Run the visual default workload**

Run `./build/pcinspector --points 10000000`.

Verify a colored spatial cloud is visible, orbit and zoom respond, resize
recreates cleanly, and metrics show GPU name, timing source, FPS, active points,
and total points.

- [ ] **Step 3: Run a larger allocation check**

Run `./build/pcinspector --smoke-test --points 50000000`.

Expected: either three rendered frames and exit code zero, or a checked
allocation failure that includes requested bytes and the returned `VkResult`.

- [ ] **Step 4: Run final verification**

Run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: configure succeeds, build exits zero, and every registered test
passes.
