# macOS QRhi Metal Proof of Concept Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the macOS `QVulkanWindow` application path with a visible `QRhiWidget` point renderer explicitly using Metal.

**Architecture:** The Qt shell owns a backend-neutral `RenderViewport` interface created by a platform factory. The macOS implementation is a `QRhiWidget` that owns QRhi buffers, shaders, bindings, pipeline, interaction state, frame timing, and draw recording; shared point generation, camera, options, metrics, and point budgeting remain API-independent.

**Tech Stack:** C++23, CMake, Qt 6.11 Widgets, Qt GuiPrivate QRhi, Qt ShaderTools, Metal through QRhi, GLSL packaged as QShader `.qsb`, CTest.

---

## File Map

- `src/renderer/RenderViewport.h`: backend-neutral viewport contract and factory.
- `src/renderer/metal/MetalRenderViewport.h/.cpp`: macOS QRhiWidget/Metal renderer.
- `src/renderer/metal/ShaderLoader.h/.cpp`: serialized QShader resource loading with testable errors.
- `src/app/MainWindow.h/.cpp`: owns the viewport interface and displays metrics/errors.
- `main.cpp`: options and backend-neutral application startup.
- `CMakeLists.txt`: Apple renderer selection, Qt ShaderTools, QShader resources, tests.
- `tests/renderer_contract_tests.cpp`: factory, API selection, and shader loader contract.
- `shaders/points.vert/.frag`: existing point shaders compiled by `qt_add_shaders`.

## Task 1: Backend-Neutral Viewport Contract

**Files:**
- Create: `src/renderer/RenderViewport.h`
- Modify: `tests/renderer_contract_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing contract test**

Add a Qt test executable whose main creates `QApplication`, calls
`pci::createRenderViewport(1000, true)`, and checks:

```cpp
CHECK(viewport != nullptr);
CHECK(viewport->backendName() == QStringLiteral("Metal"));
CHECK(viewport->widget() != nullptr);
```

- [ ] **Step 2: Verify RED**

Run `cmake -S . -B build-metal -DBUILD_TESTING=ON`.

Expected: configuration or compilation fails because the common viewport
contract and macOS factory do not exist.

- [ ] **Step 3: Add the minimal interface**

Define a virtual interface with `widget()`, `backendName()`,
`setMetricsCallback()`, and `setFailureCallback()`, plus:

```cpp
std::unique_ptr<RenderViewport>
createRenderViewport(std::uint64_t pointCount, bool smokeTest);
```

- [ ] **Step 4: Continue to Task 2**

The test remains red until the concrete Metal viewport and factory exist.

## Task 2: Metal Widget Construction and Shader Loading

**Files:**
- Create: `src/renderer/metal/MetalRenderViewport.h`
- Create: `src/renderer/metal/MetalRenderViewport.cpp`
- Create: `src/renderer/metal/ShaderLoader.h`
- Create: `src/renderer/metal/ShaderLoader.cpp`
- Modify: `tests/renderer_contract_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Extend the failing test**

Downcast `viewport->widget()` to `QRhiWidget` and require:

```cpp
CHECK(widget != nullptr);
CHECK(widget->api() == QRhiWidget::Api::Metal);
CHECK(pci::loadShaderResource(":/missing.qsb").isValid() == false);
CHECK(!pci::shaderLoadError(":/missing.qsb").isEmpty());
```

- [ ] **Step 2: Verify RED**

Build `pcinspector_renderer_contract_tests`.

Expected: failure because the widget and shader loader do not exist.

- [ ] **Step 3: Implement construction and loading**

Construct `MetalRenderViewport` with `setApi(QRhiWidget::Api::Metal)`, return
`this` from `widget()`, report `Metal`, and implement QShader deserialization
from a `QFile`, returning an invalid shader plus a precise error for missing or
invalid data.

- [ ] **Step 4: Verify GREEN for the construction contract**

Build and run the renderer contract test with
`QT_QPA_PLATFORM=offscreen`; it must pass without showing a window.

## Task 3: QRhi Point Resources and Draw Loop

**Files:**
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Modify: `shaders/points.vert`
- Modify: `shaders/points.frag`

- [ ] **Step 1: Establish a failing smoke test**

Register `pcinspector --smoke-test --points 1000` with a 20-second timeout.
Run it before resource implementation.

Expected: failure or timeout because no frames are drawn.

- [ ] **Step 2: Implement QRhi resources**

Create a 16-byte-stride immutable vertex buffer, dynamic 80-byte uniform
buffer, vertex-stage uniform binding, UShort4/UNormByte4 vertex layout,
serialized vertex/fragment shaders, and a point-topology graphics pipeline.
Reject point-buffer sizes above `quint32`.

- [ ] **Step 3: Implement chunked upload**

Generate at most one million points per chunk and immediately record each
`uploadStaticBuffer()` through `cb->resourceUpdate()`.

- [ ] **Step 4: Implement rendering**

Update the camera uniform, clear color/depth, bind pipeline/resources/vertex
buffer, draw the adaptive point prefix, end the pass, publish metrics, call
`update()`, and exit after three smoke-test frames.

- [ ] **Step 5: Implement lifecycle and errors**

Destroy QRhi objects in `releaseResources()`. Catch initialization failures,
stop updates, invoke the failure callback, and exit nonzero in smoke mode.

## Task 4: Backend-Neutral Shell and Build

**Files:**
- Modify: `src/app/MainWindow.h`
- Modify: `src/app/MainWindow.cpp`
- Modify: `main.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Replace Vulkan startup**

Remove `QVulkanInstance`; create the viewport through the factory and transfer
it into `MainWindow`.

- [ ] **Step 2: Replace native-window embedding**

Store `std::unique_ptr<RenderViewport>`, install `viewport->widget()` directly
as the central widget, and connect metrics/failure callbacks.

- [ ] **Step 3: Select Apple sources**

Require Qt Core/Gui/Widgets/ShaderTools, link `Qt6::GuiPrivate`, compile the
Metal backend only under `APPLE`, and omit the application target elsewhere.
Replace `glslc` custom commands with:

```cmake
qt_add_shaders(pcinspector point_shaders
    PREFIX "/shaders"
    FILES shaders/points.vert shaders/points.frag)
```

- [ ] **Step 4: Verify shader resources**

Build with verbose output and confirm `.qsb` generation and embedding.

## Task 5: Verification

**Files:**
- Modify only if verification identifies a defect.

- [ ] **Step 1: Run all automated tests**

Run:

```bash
cmake -S . -B build-metal -DBUILD_TESTING=ON
cmake --build build-metal --parallel
ctest --test-dir build-metal --output-on-failure
```

Expected: core, renderer contract, and Metal smoke tests pass.

- [ ] **Step 2: Run the normal application**

Launch `build-metal/pcinspector --points 1000000`, confirm the 1280-by-800
window is visible, inspect a screenshot for the colored cloud, then close it.

- [ ] **Step 3: Check dependency removal and diff**

Confirm the macOS build no longer references Vulkan, MoltenVK, or `glslc`;
review `git diff --check` and the complete diff for unrelated changes.
