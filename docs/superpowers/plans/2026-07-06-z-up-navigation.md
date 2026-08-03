# Z-Up Navigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make positive Z the default world-up axis for camera framing, orbit, panning, and Q/E movement without transforming point-cloud data.

**Architecture:** `NavigationCamera` owns the shared world-up convention and default camera basis. The Metal viewport consumes that convention when converting keyboard input to world movement; rendering and picking shaders remain unchanged because both already consume native XYZ coordinates.

**Tech Stack:** C++20, Qt 6 Widgets/QRhi, Metal, CMake, CTest

---

### Task 1: Convert the navigation camera to Z-up

**Files:**
- Modify: `src/core/NavigationCamera.h`
- Modify: `src/core/NavigationCamera.cpp`
- Test: `tests/core_tests.cpp`

- [ ] **Step 1: Update the camera tests for the Z-up basis**

Change `testNavigationDefaultsAndFraming()` to assert the complete default
basis and the framed basis:

```cpp
void testNavigationDefaultsAndFraming()
{
    const pci::Vec3d expectedPosition{0.0, -4.0, 0.0};
    const pci::Vec3d expectedForward{0.0, 1.0, 0.0};
    const pci::Vec3d expectedRight{1.0, 0.0, 0.0};
    const pci::Vec3d expectedUp{0.0, 0.0, 1.0};
    pci::NavigationCamera camera;
    CHECK(camera.position() == expectedPosition);
    CHECK(camera.forward() == expectedForward);
    CHECK(camera.right() == expectedRight);
    CHECK(camera.up() == expectedUp);
    CHECK(pci::NavigationCamera::worldUp == expectedUp);

    camera.translate({1.0, 2.0, 3.0});
    camera.frameScene();
    CHECK(camera.position() == expectedPosition);
    CHECK(camera.pivot() == pci::Vec3d{});
    CHECK(camera.forward() == expectedForward);
}
```

Update the free-translation test to use the new default viewing axis:

```cpp
void testNavigationMovesThroughPivot()
{
    pci::NavigationCamera camera;
    camera.translate({0.0, 5.0, 0.0});

    CHECK_NEAR(camera.position().y, 1.0, 1e-9);
    CHECK_NEAR(camera.pivot().y, 5.0, 1e-9);
}
```

Update the orbit assertions so horizontal orbit remains direct manipulation
and vertical orbit changes Z:

```cpp
CHECK(camera.position().x < 0.0);
CHECK(camera.position().z < 0.0);
```

Update the pan assertions for the new screen-up axis:

```cpp
CHECK_NEAR(camera.pivot().x, -0.46188, 0.0001);
CHECK_NEAR(camera.pivot().z, -0.23094, 0.0001);
CHECK_NEAR(camera.position().x, camera.pivot().x, 1e-9);
CHECK_NEAR(camera.position().z, camera.pivot().z, 1e-9);
```

- [ ] **Step 2: Build and run the core test to verify it fails**

Run:

```bash
cmake --build build-navigation --target pcinspector_core_tests
ctest --test-dir build-navigation -R '^pcinspector_core$' --output-on-failure
```

Expected: compilation fails because `NavigationCamera::worldUp` is absent, or
the test fails because the existing camera is Y-up and frames from positive Z.

- [ ] **Step 3: Define the shared Z-up basis and default camera state**

Add the public convention to `NavigationCamera`:

```cpp
static constexpr Vec3d worldUp{0.0, 0.0, 1.0};
```

Change its initial state to:

```cpp
Vec3d position_{0.0, -4.0, 0.0};
Vec3d pivot_{};
Vec3d forward_{0.0, 1.0, 0.0};
```

Remove the file-local Y-up constant from `NavigationCamera.cpp`. Change
`frameScene()` to:

```cpp
position_ = {0.0, -4.0, 0.0};
pivot_ = {};
forward_ = {0.0, 1.0, 0.0};
navigationReference_.reset();
markChanged();
```

Use `NavigationCamera::worldUp` in `right()` and both orbit rotations. Change
the pitch calculation from the Y component to the Z component:

```cpp
const double currentPitchDegrees =
    std::asin(std::clamp(-newForward.z, -1.0, 1.0))
    * 180.0 / std::numbers::pi;
```

Use the same Z-up vector when calculating the pitch axis:

```cpp
Vec3d pitchAxis =
    normalized(cross(newForward, NavigationCamera::worldUp));
```

- [ ] **Step 4: Rebuild and verify the core test passes**

Run:

```bash
cmake --build build-navigation --target pcinspector_core_tests
ctest --test-dir build-navigation -R '^pcinspector_core$' --output-on-failure
```

Expected: `pcinspector_core` passes.

- [ ] **Step 5: Commit the camera change**

```bash
git add src/core/NavigationCamera.h src/core/NavigationCamera.cpp tests/core_tests.cpp
git commit -m "feat: make navigation Z-up"
```

### Task 2: Apply Z-up to Metal keyboard navigation and contracts

**Files:**
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Test: `tests/renderer_contract_tests.cpp`
- Test: `tests/metal_point_picker_tests.cpp`

- [ ] **Step 1: Add failing renderer contract assertions**

In `tests/renderer_contract_tests.cpp`, change the F-key expectation to:

```cpp
const pci::Vec3d framedPosition{0.0, -4.0, 0.0};
CHECK(metalViewport->cameraForTesting().position()
      == framedPosition);
```

Add Q/E event assertions after focus reset:

```cpp
QKeyEvent downPress(
    QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
QApplication::sendEvent(widget, &downPress);
CHECK(metalViewport->inputForTesting().movementDirection().y < 0.0);
QKeyEvent downRelease(
    QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier);
QApplication::sendEvent(widget, &downRelease);

QKeyEvent upPress(
    QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
QApplication::sendEvent(widget, &upPress);
CHECK(metalViewport->inputForTesting().movementDirection().y > 0.0);
QKeyEvent upRelease(
    QEvent::KeyRelease, Qt::Key_E, Qt::NoModifier);
QApplication::sendEvent(widget, &upRelease);
```

In `tests/metal_point_picker_tests.cpp`, update wheel movement for a camera
starting on negative Y:

```cpp
if (viewport.cameraForTesting().position().y > -3.9) {
```

Keep the existing W movement check, then release W and add an E movement check:

```cpp
QKeyEvent release(
    QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
QApplication::sendEvent(&viewport, &release);
const auto beforeElevation =
    viewport.cameraForTesting().position();
QKeyEvent elevationPress(
    QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
QApplication::sendEvent(&viewport, &elevationPress);
```

Poll until Z has increased, release E, and exit successfully:

```cpp
elevationMoved =
    viewport.cameraForTesting().position().z
    > beforeElevation.z + 1e-4;
if (elevationMoved) {
    QKeyEvent elevationRelease(
        QEvent::KeyRelease, Qt::Key_E, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &elevationRelease);
    application.exit(0);
}
```

Declare `bool elevationMoved = false;` with the existing result flags and
include it in the final result check:

```cpp
if (!centerPicked || !emptyMissed || !wheelMoved
    || !pivotSelected || !keyboardMoved || !elevationMoved) {
    return 5;
}
```

- [ ] **Step 2: Build and run renderer tests to verify they fail**

Run:

```bash
cmake --build build-navigation --target pcinspector_renderer_contract_tests pcinspector_metal_point_picker_tests
ctest --test-dir build-navigation -R '^(renderer_contract|metal_point_picker)$' --output-on-failure
```

Expected: `renderer_contract` fails on the old framed position or
`metal_point_picker` times out because vertical input still uses positive Y.

- [ ] **Step 3: Use the camera's world-up vector for keyboard elevation**

Remove the local Y-up declaration from
`MetalRenderViewport::updateKeyboardNavigation()` and change the conversion to:

```cpp
const Vec3d worldDirection = normalized(
    camera_.right() * inputDirection.x
    + NavigationCamera::worldUp * inputDirection.y
    + camera_.forward() * inputDirection.z);
```

Do not modify `shaders/points.vert` or `shaders/pick.vert`; their native XYZ
interpretation is correct for the shared Z-up coordinate convention.

- [ ] **Step 4: Rebuild and verify the renderer tests pass**

Run:

```bash
cmake --build build-navigation --target pcinspector_renderer_contract_tests pcinspector_metal_point_picker_tests
ctest --test-dir build-navigation -R '^(renderer_contract|metal_point_picker)$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 5: Commit the renderer change**

```bash
git add src/renderer/metal/MetalRenderViewport.cpp tests/renderer_contract_tests.cpp tests/metal_point_picker_tests.cpp
git commit -m "test: verify Z-up Metal navigation"
```

### Task 3: Full verification

**Files:**
- No source changes expected

- [ ] **Step 1: Configure and build the complete Release tree**

Run:

```bash
cmake -S . -B build-navigation -DCMAKE_BUILD_TYPE=Release
cmake --build build-navigation
```

Expected: configuration and compilation complete without errors.

- [ ] **Step 2: Run the complete automated test suite**

Run:

```bash
ctest --test-dir build-navigation --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Run the one-million-point Metal smoke test**

Run:

```bash
./build-navigation/pcinspector --smoke-test --points 1000000
```

Expected: exit status 0 with no renderer failure.

- [ ] **Step 4: Check repository hygiene**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors; only the pre-existing user-owned `.gitignore`
modification remains.
