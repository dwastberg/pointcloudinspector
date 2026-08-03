# Free Point-Cloud Navigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the clamped orbit camera with unrestricted hybrid inspect/fly navigation and accurate on-demand GPU point picking.

**Architecture:** Portable core types own vector math, camera behavior, continuous movement state, and asynchronous pick intent. The Metal backend owns an isolated QRhi ID-render/readback component and connects resolved point IDs to the portable camera. The visible render path remains unchanged except for consuming the new view/projection state and advancing keyboard movement.

**Tech Stack:** C++23, Qt 6.11 Widgets/GuiPrivate/ShaderTools, QRhi Metal, GLSL compiled to QShader packages, CMake/CTest.

---

### Task 1: Deterministic single synthetic points

**Files:**
- Modify: `src/core/SyntheticPointCloud.h`
- Modify: `src/core/SyntheticPointCloud.cpp`
- Modify: `tests/core_tests.cpp`

- [ ] **Step 1: Write the failing consistency test**

Add to `testSyntheticPointGeneration()`:

```cpp
for (std::size_t offset = 0; offset < first.size(); ++offset) {
    CHECK(first[offset] == pci::generatePoint(offset));
}
CHECK(pci::generatePoint(4096)
      == pci::generatePointChunk(4096, 1).front());
```

- [ ] **Step 2: Verify the test fails because `generatePoint` is absent**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_core_tests
```

Expected: compilation fails with `no member named 'generatePoint' in namespace 'pci'`.

- [ ] **Step 3: Add the single-point API and delegate chunk generation**

Declare in `SyntheticPointCloud.h`:

```cpp
[[nodiscard]] GpuPoint generatePoint(std::uint64_t index);
```

Move the existing per-index body into:

```cpp
GpuPoint generatePoint(const std::uint64_t index)
{
    const auto x = coordinate(index, 0x243f6a8885a308d3ULL);
    const auto y = coordinate(index, 0x13198a2e03707344ULL);
    const auto z = coordinate(index, 0xa4093822299f31d0ULL);
    const auto red = static_cast<std::uint32_t>(x >> 8U);
    const auto green = static_cast<std::uint32_t>(y >> 8U);
    const auto blue = static_cast<std::uint32_t>(z >> 8U);
    return {
        .x = x,
        .y = y,
        .z = z,
        .attributes = 0,
        .rgba = red | (green << 8U) | (blue << 16U) | 0xff000000U,
        .padding = 0,
    };
}
```

Make the chunk loop call `points.push_back(generatePoint(firstIndex + offset));`.

- [ ] **Step 4: Verify core tests pass**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_core_tests
ctest --test-dir build-phase1-clean -R '^pcinspector_core$' --output-on-failure
```

Expected: build succeeds and `pcinspector_core` passes.

- [ ] **Step 5: Commit**

```bash
git add src/core/SyntheticPointCloud.h src/core/SyntheticPointCloud.cpp tests/core_tests.cpp
git commit -m "refactor: expose deterministic synthetic points"
```

### Task 2: Portable unrestricted navigation camera

**Files:**
- Create: `src/core/Vec3d.h`
- Create: `src/core/NavigationCamera.h`
- Create: `src/core/NavigationCamera.cpp`
- Delete: `src/core/OrbitCamera.h`
- Delete: `src/core/OrbitCamera.cpp`
- Modify: `tests/core_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Replace orbit-camera tests with failing navigation tests**

Include `core/NavigationCamera.h`. Add focused tests using this API:

```cpp
void testNavigationMovesThroughPivot()
{
    pci::NavigationCamera camera;
    camera.translate({0.0, 0.0, -5.0});
    CHECK_NEAR(camera.position().z, -1.0, 1e-9);
    CHECK_NEAR(camera.pivot().z, -5.0, 1e-9);
}

void testNavigationDolliesWithoutMinimumDistance()
{
    pci::NavigationCamera camera;
    const pci::Vec3d target{0.5, 0.0, 0.0};
    camera.dollyToward(target, 20.0);
    CHECK(pci::length(camera.position() - target) < 0.03);
}

void testNavigationOrbitKeepsPivotInvariant()
{
    pci::NavigationCamera camera;
    camera.setPivot({0.5, 0.25, 0.0});
    const double before = pci::length(camera.position() - camera.pivot());
    camera.orbitFromDrag(30.0, -20.0);
    CHECK_NEAR(pci::length(camera.position() - camera.pivot()), before, 1e-9);
}

void testNavigationPanAndMovementBasis()
{
    pci::NavigationCamera camera;
    camera.panFromDrag(100.0, -50.0, 1000.0);
    CHECK(camera.position().x < 0.0);
    CHECK(camera.position().y < 0.0);
    const pci::Vec3d offset = camera.position() - camera.pivot();
    CHECK_NEAR(offset.x, 0.0, 1e-9);
    CHECK_NEAR(offset.y, 0.0, 1e-9);
    CHECK_NEAR(offset.z, 4.0, 1e-9);
}

void testNavigationClipPlanesRemainFinite()
{
    pci::NavigationCamera camera;
    camera.setNavigationReference({0.0, 0.0, 3.999999});
    const auto close = camera.clipPlanes();
    CHECK(std::isfinite(close.nearPlane));
    CHECK(close.nearPlane >= pci::NavigationCamera::sceneDiameter * 1e-7);
    CHECK(close.farPlane > close.nearPlane);
}
```

Retain the existing direct-manipulation signs in dedicated orbit and pan tests.

- [ ] **Step 2: Verify RED**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_core_tests
```

Expected: compilation fails because `NavigationCamera.h` does not exist.

- [ ] **Step 3: Implement vector math and the camera**

`Vec3d.h` defines arithmetic operators plus:

```cpp
[[nodiscard]] double dot(Vec3d left, Vec3d right) noexcept;
[[nodiscard]] Vec3d cross(Vec3d left, Vec3d right) noexcept;
[[nodiscard]] double length(Vec3d value) noexcept;
[[nodiscard]] Vec3d normalized(Vec3d value) noexcept;
[[nodiscard]] Vec3d rotateAroundAxis(
    Vec3d value, Vec3d axis, double radians) noexcept;
[[nodiscard]] bool isFinite(Vec3d value) noexcept;
```

`NavigationCamera` exposes:

```cpp
class NavigationCamera {
public:
    static constexpr double sceneDiameter = 2.0;
    static constexpr double verticalFieldOfViewDegrees = 60.0;
    struct ClipPlanes { double nearPlane; double farPlane; };

    [[nodiscard]] Vec3d position() const noexcept;
    [[nodiscard]] Vec3d pivot() const noexcept;
    [[nodiscard]] Vec3d forward() const noexcept;
    [[nodiscard]] Vec3d right() const noexcept;
    [[nodiscard]] Vec3d up() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    void frameScene() noexcept;
    void setPivot(Vec3d pivot);
    void setNavigationReference(Vec3d point);
    void orbitFromDrag(double horizontalPixels, double verticalPixels);
    void panFromDrag(double horizontalPixels, double verticalPixels,
                     double viewportHeightPixels);
    void translate(Vec3d worldDelta);
    void dollyToward(Vec3d target, double wheelUnits);
    void dollyForward(double wheelUnits);
    [[nodiscard]] double movementSpeed(double multiplier = 1.0) const noexcept;
    [[nodiscard]] ClipPlanes clipPlanes() const noexcept;
};
```

Use Rodrigues rotation for yaw around `(0, 1, 0)` and pitch around camera-right.
Rotate the eye-to-pivot offset and forward direction together. Clamp resulting
forward pitch to 89.9 degrees. Every successful mutation increments `revision_`.
Reject zero viewport height and non-finite input without mutation. Implement
the wheel and clip formulas exactly as specified in the design document.

- [ ] **Step 4: Switch CMake and tests from `OrbitCamera` to `NavigationCamera`**

Replace `src/core/OrbitCamera.cpp` in `pcinspector_core` with
`src/core/NavigationCamera.cpp`; remove the old files after no source includes
remain.

- [ ] **Step 5: Verify GREEN**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_core_tests
ctest --test-dir build-phase1-clean -R '^pcinspector_core$' --output-on-failure
```

Expected: all core camera and existing core tests pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core tests/core_tests.cpp
git commit -m "feat: add unrestricted navigation camera"
```

### Task 3: Continuous movement and pick-intent state

**Files:**
- Create: `src/core/NavigationInputState.h`
- Create: `src/core/NavigationInputState.cpp`
- Create: `tests/navigation_input_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing movement-state tests**

Create tests for this public contract:

```cpp
pci::NavigationInputState input;
input.press(pci::MovementKey::Forward);
input.press(pci::MovementKey::Right);
const auto direction = input.movementDirection();
CHECK_NEAR(pci::length(direction), 1.0, 1e-9);
input.setFast(true);
CHECK_NEAR(input.speedMultiplier(), 5.0, 1e-9);
input.setFine(true);
CHECK_NEAR(input.speedMultiplier(), 0.5, 1e-9);
input.clearMovement();
CHECK(pci::length(input.movementDirection()) == 0.0);
CHECK_NEAR(pci::NavigationInputState::boundedDeltaSeconds(2.0), 0.1, 1e-9);
```

The returned direction uses `(right, vertical, forward)` components.

- [ ] **Step 2: Write failing request-state tests**

```cpp
input.queueWheel({10, 20}, 1.0);
input.queueWheel({12, 22}, 2.0);
const auto request = input.takePendingPick(7);
CHECK(request->kind == pci::PickKind::Wheel);
CHECK_NEAR(request->wheelUnits, 3.0, 1e-9);
CHECK(request->cameraRevision == 7);
CHECK(!input.takePendingPick(7));

input.queueWheel({1, 2}, 1.0);
input.queuePivot({5, 6});
CHECK(input.pendingPick()->kind == pci::PickKind::Pivot);

input.markStale(*request);
CHECK(input.pendingPick()->kind == pci::PickKind::Wheel);
```

- [ ] **Step 3: Verify RED**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_navigation_input_tests
```

Expected: target or source types are missing.

- [ ] **Step 4: Implement input and request state**

Define `MovementKey`, `PickKind`, `PixelPosition`, and `PickRequest` as small
value types. `NavigationInputState` owns six movement booleans, fast/fine
flags, one optional pending request, and one optional in-flight request.
Normalize the three movement components. Clamp delta seconds to `[0, 0.1]`.
Wheel requests sum units and retain the latest cursor. Pivot requests replace
pending wheel requests. `takePendingPick(revision)` moves pending to in-flight.
`completeInFlight()` clears it; `markStale()` requeues unresolved intent unless
a newer pivot request exists.

- [ ] **Step 5: Add and pass the dedicated CTest target**

Add `pcinspector_navigation_input_tests`, link `pcinspector_core`, and
register it as `navigation_input`.

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_navigation_input_tests
ctest --test-dir build-phase1-clean -R '^navigation_input$' --output-on-failure
```

Expected: `navigation_input` passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core/NavigationInputState.* tests/navigation_input_tests.cpp
git commit -m "feat: add navigation input state"
```

### Task 4: QRhi point-ID picker

**Files:**
- Create: `shaders/pick.vert`
- Create: `shaders/pick.frag`
- Create: `src/renderer/metal/MetalPointPicker.h`
- Create: `src/renderer/metal/MetalPointPicker.cpp`
- Create: `tests/point_pick_result_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing CPU-side result-decoding tests**

Test a public static decoder using a 5-by-5 `R32UI` byte array. Put IDs at
different distances from the center and assert the nearest nonzero ID wins;
assert an all-zero region returns `std::nullopt`; assert malformed byte counts
return `std::nullopt`.

- [ ] **Step 2: Verify RED**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_point_pick_result_tests
```

Expected: compilation fails because `MetalPointPicker.h` is absent.

- [ ] **Step 3: Implement and verify the decoder**

Expose:

```cpp
[[nodiscard]] static std::optional<std::uint32_t> decodeNearestId(
    const QByteArray &data, QSize size);
```

Interpret each four-byte texel with `memcpy`, ignore zero, minimize squared
pixel distance to `(width - 1, height - 1) / 2`, and return `storedId - 1`.

Run the decoder test and expect PASS before adding GPU resources.

- [ ] **Step 4: Add point-ID shaders**

`pick.vert` reconstructs the same normalized position as `points.vert`, uses
the same camera uniform block, assigns `gl_PointSize`, and emits:

```glsl
layout(location = 0) flat out uint pointId;
pointId = uint(gl_VertexIndex) + 1u;
```

`pick.frag` declares the matching flat input and:

```glsl
layout(location = 0) out uint outputId;
outputId = pointId;
```

Add both shaders to the existing `qt_add_shaders` target.

- [ ] **Step 5: Implement QRhi resource ownership**

`MetalPointPicker` receives `QRhi*` and shared camera shader bindings. It:

1. verifies `R32UI` with `RenderTarget | UsedAsTransferSource`;
2. creates an `R32UI` texture, depth renderbuffer, texture target, render-pass
   descriptor, and point graphics pipeline for the requested pixel size;
3. records the ID pass with the shared point buffer and active point count;
4. requests a clamped 5-by-5 `QRhiReadbackDescription::rect`;
5. owns `QRhiReadbackResult` until its completion callback returns the decoded
   optional zero-based vertex ID; and
6. deletes pipeline, descriptor, target, depth, and texture in dependency order.

Use the same `GpuPoint` vertex input layout and `Less` depth operation as the
visible pipeline. Reject a second record call while a readback is in flight.

- [ ] **Step 6: Build shader packages and decoder tests**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector pcinspector_point_pick_result_tests
ctest --test-dir build-phase1-clean -R '^point_pick_result$' --output-on-failure
```

Expected: shader baking succeeds and decoder tests pass.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt shaders/pick.* src/renderer/metal/MetalPointPicker.* tests/point_pick_result_tests.cpp
git commit -m "feat: add Metal point ID picker"
```

### Task 5: Viewport navigation integration

**Files:**
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Modify: `tests/renderer_contract_tests.cpp`

- [ ] **Step 1: Write failing renderer input contract tests**

Construct `MetalRenderViewport` directly and use Qt events to verify:

- it accepts focus;
- `F` restores the camera after a test translation;
- press/release changes movement state without auto-repeat;
- focus-out clears held movement; and
- loading a cloud resets the camera revision and frames the scene.

Expose read-only `cameraForTesting()` and `inputForTesting()` accessors only in
the concrete backend; the cross-platform `RenderViewport` interface remains
unchanged.

- [ ] **Step 2: Verify RED**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_renderer_contract_tests
```

Expected: compilation fails because the new camera/input accessors and handlers
are absent.

- [ ] **Step 3: Replace orbit view construction**

Store `NavigationCamera`, `NavigationInputState`, and `MetalPointPicker`.
Construct the view with:

```cpp
view.lookAt(toQVector(camera_.position()),
            toQVector(camera_.position() + camera_.forward()),
            toQVector(camera_.up()));
```

Use `camera_.clipPlanes()` for perspective near/far values. Retain
`rhi()->clipSpaceCorrMatrix()`.

- [ ] **Step 4: Integrate mouse, wheel, and keyboard events**

Keep left/right drag selection. Send drag deltas to camera orbit/pan. Add
double-click to queue a pivot pick. Queue wheel units and cursor coordinates
instead of mutating the camera immediately. Map `W/S/A/D/Q/E`, `Shift`,
`Option`/`Alt`, and `F`; ignore auto-repeat; clear movement on focus-out.

At each render, use a monotonic timer, cap delta through
`NavigationInputState`, convert the normalized movement intent through camera
right/world-up/forward, and translate by
`camera_.movementSpeed(multiplier) * deltaSeconds`.

- [ ] **Step 5: Integrate asynchronous picking**

Before the visible pass, submit a pending request when no readback is active.
Convert logical cursor coordinates with the current render-target/logical size
ratio. Capture request and camera revision in the completion callback.

If the revision is stale, requeue the intent. Otherwise decode the selected
`GpuPoint` from `pointCloud_->points[id]` or `generatePoint(id)`, map each
coordinate with `value / 65535.0 * 2.0 - 1.0`, and apply pivot or wheel action.
A miss applies forward dolly only for wheel requests. Schedule another update.

- [ ] **Step 6: Handle lifecycle and scene replacement**

Ensure/recreate picker resources from `initialize()` after visible bindings
exist. Release picker before shared buffers/bindings. On point-cloud
replacement, cancel pending intents, frame the scene, and invalidate stale
results. Keep smoke-test frame counting independent of pick passes.

- [ ] **Step 7: Verify core and renderer contracts**

Run:

```bash
cmake --build build-phase1-clean
ctest --test-dir build-phase1-clean -R '^(pcinspector_core|navigation_input|point_pick_result|renderer_contract)$' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/renderer/metal/MetalRenderViewport.* tests/renderer_contract_tests.cpp
git commit -m "feat: integrate free point cloud navigation"
```

### Task 6: Metal picking integration smoke test

**Files:**
- Create: `tests/metal_point_picker_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing visible-GPU test**

Create a `QApplication`, a visible `MetalRenderViewport`, and a
`LoadedPointCloud` containing one point at quantized center. Add a narrow
concrete-backend test hook that requests a raw pick and supplies the optional ID
to a callback without changing camera state. Show a 320-by-240 viewport,
request its center after initialization, and use a five-second timer to fail if
the callback does not return ID zero.

Then replace the cloud with one point in a corner, pick the center, and require
`std::nullopt`.

- [ ] **Step 2: Verify RED under WindowServer**

Run:

```bash
cmake --build build-phase1-clean --target pcinspector_metal_point_picker_tests
ctest --test-dir build-phase1-clean -R '^metal_point_picker$' --output-on-failure
```

Expected: test fails before the raw-pick hook is connected to the actual picker.

- [ ] **Step 3: Connect the test hook to the production picker request path**

The hook queues the same `PickRequest` and completion flow as wheel/double-click
input but returns the resolved optional ID before camera action. It must not
provide a separate CPU picking path.

- [ ] **Step 4: Verify GREEN**

Run the same build and CTest commands. Expected: the Metal test returns ID zero
for center and no ID for empty center.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/metal_point_picker_tests.cpp src/renderer/metal/MetalRenderViewport.*
git commit -m "test: verify Metal point picking"
```

### Task 7: Full regression and manual acceptance

**Files:**
- Modify only if a failing test exposes a defect; every correction starts with
  a focused failing regression test in the owning test file.

- [ ] **Step 1: Clean configure and build**

Run:

```bash
cmake -S . -B build-navigation -DCMAKE_BUILD_TYPE=Release
cmake --build build-navigation
```

Expected: configure and build succeed with all four QShader packages generated.

- [ ] **Step 2: Run the complete automated suite**

Run under macOS WindowServer:

```bash
ctest --test-dir build-navigation --output-on-failure
```

Expected: all existing tests plus `navigation_input`, `point_pick_result`, and
`metal_point_picker` pass.

- [ ] **Step 3: Run the application and perform manual acceptance**

Run:

```bash
./build-navigation/pcinspector --points 1000000
```

Verify wheel targeting, off-center double-click orbit, right pan, all six
movement keys, both modifiers, movement through the cloud, close inspection,
`F`, resize, and focus-loss behavior. Load LAS/LAZ/COPC fixtures and confirm
each new cloud frames correctly.

- [ ] **Step 4: Check the final diff and worktree**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors. The only unrelated unstaged change remains the
user-owned `.gitignore` edit.

- [ ] **Step 5: Commit any test-driven corrections**

Stage only files belonging to this feature and commit with a message describing
the verified correction. Do not stage `.gitignore`.
