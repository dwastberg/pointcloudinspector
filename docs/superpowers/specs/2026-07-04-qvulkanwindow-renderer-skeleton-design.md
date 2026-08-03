# QVulkanWindow Renderer Skeleton Design

## Purpose

Build a narrow Phase 1 proof of concept that displays and benchmarks a large
synthetic point cloud. The result establishes Qt/Vulkan integration and a
measurable point-rendering baseline. It does not establish the production
renderer architecture.

The proof of concept succeeds when it:

- opens a Qt Widgets main window containing a native Vulkan viewport;
- displays a colored synthetic cloud containing 10 million points by default;
- accepts a larger point count through a command-line option;
- supports mouse orbit and wheel zoom;
- reports GPU frame time, frames per second, active point count, total point
  count, and GPU name;
- adjusts the active point count toward a 16.67 ms GPU frame target; and
- shuts down and recreates swapchain-dependent resources without Vulkan
  validation errors.

## Scope

### Included

- A `QMainWindow` shell.
- A `QVulkanWindow` embedded with `QWidget::createWindowContainer()`.
- One Vulkan graphics pipeline using point-list topology.
- Deterministic, synthetic, quantized points generated in CPU chunks.
- One device-local point buffer populated through a staging buffer.
- Per-frame camera uniforms.
- Vulkan timestamp queries with a CPU timing fallback.
- A small adaptive point-budget controller.
- Status-bar benchmark metrics.
- Unit tests for non-Vulkan logic.

### Deferred

- A custom `QWindow`, device, or swapchain implementation.
- A dedicated render thread.
- Runtime file loading or point-cloud import.
- Out-of-core streaming, hierarchy traversal, LOD selection, page caching, or
  residency management.
- Compute culling, indirect drawing, bindless resources, or multiple queues.
- Splats, EDL, picking, selection, measurement, and editing tools.
- D3D12, Metal, QRhi, or a generalized renderer abstraction.

## Architecture

`main.cpp` creates the application-wide `QVulkanInstance`, enables the standard
validation layer when it is installed in a debug build, and constructs
`MainWindow`. Failure to create the Vulkan instance is reported to the user and
terminates the application with a nonzero status.

`MainWindow` embeds `RenderViewport`, a small `QVulkanWindow` subclass, using a
native window container. It displays renderer metrics in its status bar.

`RenderViewport` owns camera input state and creates `VulkanPointRenderer`
through `createRenderer()`. It forwards mouse drag and wheel input into an
orbit camera and exposes renderer metrics to the Qt shell.

`VulkanPointRenderer` implements `QVulkanWindowRenderer`. Qt owns the Vulkan
device, graphics queue, command pool, swapchain, default render pass,
framebuffers, depth image, and primary command buffers. The renderer owns:

- the point graphics pipeline and shader modules;
- point, staging, and per-frame uniform buffers;
- descriptor set layout, pool, and per-frame descriptor sets;
- timestamp query storage; and
- the commands recorded into `QVulkanWindow::currentCommandBuffer()`.

The first pass intentionally runs renderer callbacks on Qt's GUI thread, as
defined by `QVulkanWindow`. Resource creation may block startup, but rendering
must not perform point generation or allocation after initialization.

The pure C++ `SyntheticPointCloud` and `AdaptivePointBudget` modules do not
depend on Qt or Vulkan. This keeps the two behaviors that need iteration
directly unit-testable without a GPU.

## Build Prerequisites

The build requires:

- C++23;
- CMake;
- Qt 6 with Widgets, Gui, Test, and Vulkan support;
- Vulkan headers and loader;
- MoltenVK when building on macOS; and
- `glslc` for build-time GLSL-to-SPIR-V compilation.

CMake must fail during configuration with a direct diagnostic when Vulkan or
`glslc` is unavailable. Shader compilation is part of the normal build, and
the generated SPIR-V files are embedded as Qt resources.

The current machine has Qt 6.11.1 with Vulkan-enabled Qt headers, but its
existing CMake cache does not find Vulkan headers, the Vulkan loader, or a GLSL
compiler. Those dependencies must be installed before the proof of concept can
be built and run.

## Point Representation and Generation

The GPU point type is exactly 16 bytes:

```cpp
struct GpuPoint {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t z;
    std::uint16_t attributes;
    std::uint32_t rgba;
    std::uint32_t padding;
};
```

Positions cover the full unsigned 16-bit range inside a fixed local bounding
box. The vertex shader converts them to normalized local coordinates and then
applies the camera matrix. Color is stored as four normalized unsigned bytes.
The unused fields preserve the intended extensible 16-byte Phase 1 layout.

The generator produces fixed-size CPU chunks so generation does not require a
second full-sized CPU copy. Each point derives from its global index through a
stable integer hash. Consequently, any prefix of the generated buffer remains
distributed over the whole cloud, allowing the adaptive controller to vary draw
count without revealing generation order.

The default requested point count is 10,000,000. `--points <count>` accepts a
positive integer. Allocation or upload failure is reported with the requested
byte count and the Vulkan result rather than silently lowering the dataset
size.

## Frame Data Flow

Initialization performs this sequence:

1. Generate one CPU chunk.
2. Copy the chunk into a reusable host-visible staging buffer.
3. Submit a one-time copy into the corresponding region of the final
   device-local point buffer.
4. Repeat until all requested points are resident.
5. Create per-frame uniform and descriptor resources.
6. Create timestamp queries and the graphics pipeline.

Each frame performs this sequence:

1. Retrieve an available timestamp result from the previous use of the current
   frame slot.
2. Feed that duration into the adaptive point-budget controller.
3. Update the current frame's camera uniform buffer.
4. Reset and write the frame's timestamp queries.
5. Begin Qt's default render pass and clear color and depth.
6. Bind the pipeline, point buffer, and current descriptor set.
7. Issue one `vkCmdDraw(activePointCount, 1, 0, 0)`.
8. End the render pass, write the ending timestamp, call `frameReady()`, and
   request another update.

If timestamps are unsupported or unavailable, elapsed CPU frame time drives
the budget and the metrics label identifies the timing source.

## Camera and Interaction

The camera is an orbit camera centered on the synthetic cloud:

- left-button drag changes yaw and pitch;
- pitch is clamped short of the poles;
- wheel input changes distance with positive lower and upper bounds; and
- resize updates the projection aspect ratio.

The uniform transform uses `QVulkanWindow::clipCorrectionMatrix()` so the
camera code can use Qt's conventional projection matrices.

## Adaptive Point Budget

The controller receives the latest measured frame duration and returns the
next draw count. It starts at the smaller of one million points and the total
point count.

- Above 18.34 ms, reduce the budget by 10%.
- Below 13.34 ms, increase the budget by 10%.
- Between those thresholds, keep the budget unchanged.
- Clamp the result between 100,000 points and the total point count, using the
  total count as the lower clamp when the dataset contains fewer than 100,000
  points.

The dead band prevents constant budget changes near the 16.67 ms target. This
is intentionally a simple baseline rather than the production quality policy.

## Metrics

Metrics are published no more than four times per second and contain:

- physical-device name;
- exponentially smoothed FPS;
- exponentially smoothed frame time in milliseconds;
- timing source (`GPU` or `CPU`);
- active point count; and
- total point count.

Metrics are display-only and never feed back into Vulkan resource ownership.
The adaptive controller receives raw frame durations independently.

## Resource Lifetime and Errors

Swapchain-dependent state is limited to pipeline objects whose compatibility
depends on Qt's default render pass. It is recreated in
`initSwapChainResources()` and destroyed in `releaseSwapChainResources()`.
Device-level buffers, descriptors, shaders, and queries are created in
`initResources()` and destroyed in reverse order in `releaseResources()`.

Every Vulkan creation, allocation, mapping, and queue-submission result is
checked. Initialization failure records a precise diagnostic, leaves no
partially owned resources, and stops continuous rendering. Vulkan instance
failure is handled before the window is shown.

## Testing and Acceptance

Automated tests cover:

- the 16-byte GPU point layout;
- deterministic generation for a known index range;
- generated coordinates spanning multiple regions of the quantized domain;
- initial point-budget selection;
- budget increase, decrease, dead-band behavior, and clamps; and
- command-line rejection of zero, negative, malformed, or overflowing counts.

The implementation is accepted after:

1. configuration and compilation complete with shaders generated;
2. all automated tests pass;
3. the application displays a colored three-dimensional cloud;
4. orbit, zoom, and resize behave correctly;
5. status metrics update while rendering continuously;
6. increasing `--points` changes the reported total and GPU allocation;
7. the active budget changes in response to measured frame time; and
8. a debug run produces no Vulkan validation errors during startup, resize,
   rendering, and shutdown.
