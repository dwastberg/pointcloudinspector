# Right-Drag Camera Pan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add distance-scaled, screen-space camera panning with right-button drag.

**Architecture:** `OrbitCamera` stores and updates an API-independent world-space focus point. `MetalRenderViewport` selects one drag mode, forwards right-drag deltas and logical viewport height to the camera, and constructs its eye/look-at pair around the translated focus.

**Tech Stack:** C++23, Qt 6 mouse events and QVector3D conversion at the renderer boundary, CTest.

---

## Task 1: Camera Pan Math

**Files:**
- Modify: `tests/core_tests.cpp`
- Modify: `src/core/OrbitCamera.h`
- Modify: `src/core/OrbitCamera.cpp`

- [ ] Write failing tests for default right/up movement, yaw-rotated axes,
  distance scaling, and zero-height handling.
- [ ] Run `cmake --build build-metal-verify --target pcinspector_core_tests`
  and confirm compilation fails because `panFromDrag()` and focus getters are
  absent.
- [ ] Add `panFromDrag(horizontalPixels, verticalPixels, viewportHeight)`,
  `focusX()`, `focusY()`, and `focusZ()`. Compute world units per pixel as
  `2 * distance * tan(30 degrees) / viewportHeight`; translate focus by
  `-right * dx + up * dy`.
- [ ] Run the core tests and confirm all pass.

## Task 2: Metal Viewport Input and View Matrix

**Files:**
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`

- [ ] Replace the orbit boolean with `DragMode { None, Orbit, Pan }`.
- [ ] Select orbit on left press and pan on right press; only release the
  currently active matching mode.
- [ ] Route pan movement through `panFromDrag()` using QWidget logical height.
- [ ] Add the focus vector to both eye and look-at target in `cameraUniform()`.

## Task 3: Verification

**Files:**
- Modify only if verification finds a defect.

- [ ] Run the full build and non-GUI tests.
- [ ] Run the real Metal smoke test with display access.
- [ ] Run a warning-enabled build and `git diff --check`.
