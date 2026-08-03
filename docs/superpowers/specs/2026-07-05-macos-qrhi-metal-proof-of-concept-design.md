# macOS QRhi Metal Proof of Concept Design

## Purpose

Replace the non-displaying macOS `QVulkanWindow` proof of concept with a
`QRhiWidget` renderer explicitly configured for Metal. The result remains a
narrow visualization and benchmarking baseline: it renders the existing
synthetic point cloud, preserves camera interaction and adaptive point
budgeting, and reports basic timing metrics.

This pass targets macOS only. It establishes an application-facing viewport
boundary and platform-specific source layout so Windows and Linux renderers can
be added without changing the main window or core point-cloud code.

The proof of concept succeeds when it:

- opens a visible Qt Widgets main window on macOS;
- reports that the selected QRhi implementation is Metal;
- displays the existing colored synthetic cloud with 10 million points by
  default;
- preserves `--points`, orbit, zoom, adaptive point count, status metrics, and
  `--smoke-test`;
- builds without Vulkan, MoltenVK, or `glslc`; and
- exits cleanly after three submitted frames in smoke-test mode.

## Scope

### Included

- A macOS `QRhiWidget` viewport forced to `QRhiWidget::Api::Metal`.
- A Metal-backed QRhi graphics pipeline using point-list topology.
- Build-time Qt Shader Tools compilation of Vulkan-style GLSL into `.qsb`
  shader packages containing Metal Shading Language output.
- Reuse of `GpuPoint`, `SyntheticPointCloud`, `OrbitCamera`,
  `AdaptivePointBudget`, and `RenderMetrics`.
- A small application-facing renderer interface and platform factory.
- Chunked upload into one immutable QRhi vertex buffer.
- Per-frame camera uniform updates.
- QRhi/Metal GPU timing when available, with a labeled CPU frame-time fallback.
- Explicit CMake platform selection that currently enables only the macOS
  renderer.

### Deferred

- Native Metal or Metal-cpp resource and command ownership.
- A direct `CAMetalLayer` or `MTKView` integration.
- Vulkan restoration on Windows or Linux.
- A D3D12 backend.
- A general low-level graphics abstraction.
- Render threads, asynchronous upload, streaming, LOD, compute culling,
  indirect commands, residency management, and bindless resources.
- File loading and production point-cloud formats.

## Architecture

`RenderViewport` becomes a small non-Qt interface used by the application
shell. It exposes the backend's `QWidget`, accepts a metrics callback, and
reports the selected backend name. A platform factory creates the concrete
viewport. `MainWindow` owns this interface and installs its widget directly as
the central widget; it no longer knows about `QWindow`,
`QWidget::createWindowContainer()`, Vulkan, or Metal.

The macOS implementation, `MetalRenderViewport`, derives from both
`QRhiWidget` and `RenderViewport`. Its constructor selects
`QRhiWidget::Api::Metal` before the widget joins a visible hierarchy. It owns
camera input state and all QRhi resources:

- immutable point vertex buffer;
- dynamic camera uniform buffer;
- shader resource bindings;
- point-list graphics pipeline; and
- loaded vertex and fragment `QShader` packages.

`main.cpp` parses the existing command-line options, calls the platform
viewport factory, and constructs `MainWindow`. It performs no graphics-API
initialization.

Platform-specific renderer files live below `src/renderer/metal`. The common
factory declaration and application interface do not contain QRhi, Metal, or
Vulkan types. Future Windows and Linux work can provide different factory
implementations and backend directories while retaining the same Qt shell and
core modules.

QRhi itself has limited source and binary compatibility guarantees. The build
therefore treats the Qt minor release as a renderer dependency and confines all
`<rhi/qrhi.h>` usage to the Metal backend implementation.

## Build Structure

The macOS application requires:

- C++23;
- CMake 3.24 or newer;
- Qt 6.7 or newer with Core, Gui, Widgets, GuiPrivate, and ShaderTools; and
- macOS Metal support in the installed Qt build.

Vulkan discovery, MoltenVK, and the external `glslc` program are removed from
the macOS configuration. `qt_add_shaders` invokes Qt Shader Tools and embeds
the resulting `.qsb` files under `/shaders`.

CMake selects renderer sources by platform. On Apple platforms it compiles the
Metal backend and defines the platform factory. On other platforms it omits the
application target, emits a direct configuration status message, and retains
the portable core-test target. This avoids accidentally presenting the macOS
backend as portable while preserving an obvious insertion point for Windows
and Linux.

## Point and Shader Contract

The existing 16-byte `GpuPoint` layout remains unchanged. QRhi describes it as
one vertex binding with:

- location 0: four unsigned 16-bit integers at byte offset 0; and
- location 1: four normalized unsigned bytes at byte offset 8.

The vertex shader reconstructs the quantized position into `[-1, 1]`, applies
the camera matrix, assigns point size, and forwards color. The fragment shader
outputs that color. The shader interface retains the existing uniform block:
a 4-by-4 matrix followed by point size and explicit padding.

Qt Shader Tools compiles the GLSL once at build time and packages the generated
MSL together with reflection metadata. Runtime code loads only the serialized
`QShader` resources.

QRhi buffer sizes and non-indexed draw counts are 32-bit. Initialization rejects
requests whose point count or 16-byte buffer size cannot be represented by the
corresponding QRhi API parameters and reports the requested count and byte size.

## Resource and Frame Flow

During the first `initialize()` call for a QRhi instance:

1. Verify that QRhi reports the Metal implementation and integer vertex
   attributes.
2. Create an immutable vertex buffer sized for all requested points.
3. Generate deterministic point chunks and record static-buffer uploads without
   retaining a second full point-cloud copy.
4. Create the dynamic uniform buffer and shader resource bindings.
5. Load both shader packages and create a point-list graphics pipeline
   compatible with the widget's render target.
6. Record the device name and initialize frame timing state.

Subsequent `initialize()` calls caused only by render-target resize reuse
device-level buffers and rebuild the pipeline only when its render-pass
descriptor compatibility changes. A changed QRhi instance triggers full
resource recreation.

Each `render()` call:

1. Read the latest completed Metal GPU duration if QRhi supplies a positive
   value; otherwise calculate CPU frame duration.
2. Update the adaptive point budget from that duration.
3. Build the view-projection matrix using `QRhi::clipSpaceCorrMatrix()` and the
   current widget pixel aspect ratio.
4. Upload the current camera uniform.
5. Begin a pass that clears the color and depth targets.
6. Bind the pipeline, resources, and point vertex buffer.
7. Draw the active point prefix with one non-indexed point-list draw.
8. End the pass, publish rate-limited metrics, and schedule another widget
   update.

Smoke-test mode counts submitted render calls and exits successfully after
three frames.

## Interaction and Metrics

The viewport handles left-button orbit and wheel zoom directly as a QWidget.
Input modifies the existing `OrbitCamera` and schedules an update. Resize is
handled by `QRhiWidget`; the projection aspect ratio is derived from the current
render-target pixel size rather than logical widget dimensions.

Metrics retain the existing fields. `deviceName` comes from
`QRhi::driverInfo()`. `timingSource` is `Metal GPU` when
`QRhiCommandBuffer::lastCompletedGpuTime()` is positive and `CPU` otherwise.
The adaptive controller and displayed values use the same selected duration.
Publication remains limited to four updates per second.

GPU timing from this path measures QRhi's completed frame and may include work
outside the point draw. It is suitable for proof-of-concept trend comparison,
not for cross-API benchmark claims.

## Errors and Lifetime

Failure to create buffers, load shaders, create bindings, or create the
pipeline marks the viewport failed, logs a precise diagnostic, stops continuous
updates, and reports the error in the main-window status bar. Smoke-test mode
exits nonzero on renderer initialization or submission failure.

`releaseResources()` destroys pipeline and binding objects before buffers and
clears the QRhi identity used to detect device recreation. All QRhi resources
remain owned by the viewport and are destroyed only from QRhiWidget lifecycle
callbacks.

`MainWindow` owns the `RenderViewport` object. Because the concrete viewport is
also the central QWidget, deleting it removes itself from the Qt child list
before the `QMainWindow` base destructor runs.

## Testing and Acceptance

Existing core tests continue to cover point generation, point layout, camera
limits, option parsing, and adaptive budgeting.

New automated checks cover:

- the platform factory reporting `Metal`;
- construction selecting `QRhiWidget::Api::Metal` before display;
- shader packages being generated and embedded;
- clean failure when a shader resource is missing; and
- the macOS smoke test completing three rendered frames within its timeout.

Acceptance requires:

1. a clean macOS configure and build without Vulkan or `glslc`;
2. all core and renderer tests passing;
3. a visible 1280-by-800 main window;
4. a rendered colored cloud at the default point count;
5. functional orbit, zoom, and resize;
6. status metrics naming Metal and the selected Apple GPU;
7. `--points` changing allocation and total-point reporting; and
8. clean normal and smoke-test shutdown.
