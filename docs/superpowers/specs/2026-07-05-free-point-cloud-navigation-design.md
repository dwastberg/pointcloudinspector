# Free Point-Cloud Navigation Design

## Purpose

Replace the distance-limited orbit camera with a hybrid inspection and
free-flight camera. A user must be able to select any visible point, orbit
around it, move toward it with the wheel, fly through the cloud, and inspect it
at the practical precision limit of the current point representation.

The current camera cannot do this because it derives the eye position from a
focus point and clamps their distance to `1.25`. The rendered cloud occupies
approximately `[-1, 1]`, so the clamp prevents the camera from entering most of
the cloud. Removing only the clamp would make crossing the focus point unstable
and would leave the fixed near plane as another close-viewing limit.

This work changes navigation and picking only. It does not change point
storage, loading, level of detail, point styling, or the platform renderer
strategy.

## Interaction Contract

The viewport uses these controls:

- Left-drag orbits around the current pivot.
- Right-drag pans in the camera's screen plane.
- Double-click selects the actual visible point under the cursor as the orbit
  pivot.
- Wheel input selects the actual visible point under the cursor and dollies
  along the camera-to-point ray. A miss dollies along camera-forward instead.
- `W` and `S` move forward and backward.
- `A` and `D` move left and right.
- `Q` and `E` move down and up along world vertical.
- Holding `Shift` multiplies keyboard speed by five.
- Holding `Option`/`Alt` multiplies keyboard speed by one tenth.
- `F` frames the complete cloud.

Keyboard movement is continuous and time-based. Simultaneous direction keys
produce a normalized direction, so diagonal movement is not faster. Input
auto-repeat is ignored. Losing viewport focus clears all held keys, and the
per-frame movement delta is capped at 100 milliseconds to prevent a jump after
a pause.

Loading a new cloud frames it automatically. Synthetic and Phase 1 imported
clouds use the same normalized render space, whose nominal scene diameter is
two world units.

## Camera Model

Replace `OrbitCamera` with an API-independent `NavigationCamera` in the core
library. It owns three independent pieces of state:

- a world-space camera position;
- a world-space orientation with no roll; and
- a world-space orbit pivot.

The initial view retains the current appearance: position `(0, 0, 4)`, pivot
`(0, 0, 0)`, a 60-degree vertical field of view, and camera-forward toward
negative Z.

The model exposes camera basis vectors, a view transform description, framing,
orbit, pan, dolly, and local/world translation operations. Core code uses its
own small vector and orientation types rather than Qt or graphics-API types so
the same navigation semantics can be reused by future Windows and Linux
renderers.

Orbit rotates both the eye-to-pivot vector and camera orientation by the same
world-up and camera-right rotations. The pivot therefore stays at the same
screen-relative location instead of snapping to the center when a newly picked
off-center point is orbited. Pitch remains within 89.9 degrees of world
vertical to avoid the world-up singularity; this restricts orientation only,
not camera position.

Pan translates position and pivot together. Its world-units-per-pixel scale is
derived from the 60-degree field of view, viewport height, and eye-to-pivot
distance. A scene-diameter-scaled numerical floor keeps pan defined if the eye
and pivot coincide.

Keyboard movement also translates position and pivot together, preserving the
current inspection setup. Its base speed in world units per second is the
eye-to-pivot distance clamped between `sceneDiameter * 1e-5` and
`sceneDiameter * 2`. The speed modifiers are applied after this calculation.
The lower speed is below the current 16-bit coordinate quantization interval,
allowing meaningful fine positioning without introducing a camera-distance
barrier. `W` movement may cross and continue beyond the pivot.

Wheel dolly does not alter the orbit pivot. For a successful pick, it moves the
camera along the exact eye-to-picked-point ray while leaving orientation
unchanged, so the point stays under the cursor. Its exponential scale is based
on eye-to-point distance and has no fixed minimum distance. For wheel input
`u`, measured in 120-angle-delta units, target `T`, and eye `E`, the new eye is:

```text
E' = T + (E - T) * exp(-0.25 * u)
```

Positive `u` moves closer. Repeated wheel input can approach the point to
numerical precision; keyboard movement is used to pass through it. If
`angleDelta` is unavailable, `pixelDelta / 120` supplies `u`. A miss uses the
eye-to-pivot distance `s` as the movement scale and translates the eye by:

```text
cameraForward * s * (1 - exp(-0.25 * u))
```

## Adaptive Projection

There is no intentional minimum camera or eye-to-pivot distance.

The renderer computes the near plane from the nearest current navigation
reference: the last valid wheel target when one is active, otherwise the orbit
pivot. It uses one percent of that distance, clamped to:

```text
[sceneDiameter * 1e-7, sceneDiameter * 0.005]
```

The far plane is at least four scene diameters and otherwise expands to contain
the normalized cloud bounds from the camera's current position. It is always
greater than the near plane. Projection inputs are checked for finite values
before constructing the matrix.

This removes the current `0.01` near-plane obstacle while retaining useful
depth precision. Ultimate close-viewing precision is limited by floating-point
projection and the existing unsigned 16-bit point coordinates. Those are data
and numerical limits, not navigation-policy clamps.

## GPU Point Picking

Picking uses an on-demand Metal/QRhi render pass rather than scanning points on
the CPU. The installed QRhi version supports `R32UI` render-target textures,
rectangular asynchronous texture readback, and transfer-source textures.

The Metal viewport owns:

- an `R32UI` point-ID texture with `UsedAsTransferSource`;
- a depth attachment;
- a texture render target and compatible render-pass descriptor; and
- a point-ID graphics pipeline and shaders.

These resources match the viewport's physical-pixel size and are recreated
after a size change. They are released through the existing QRhi lifecycle.
Initialization verifies `R32UI` render-target support. Unsupported format or
resource creation follows the renderer's existing failure path because
accurate targeting is part of the navigation contract.

For a pick, the renderer clears the ID target to zero and draws the same active
point prefix with the same camera matrix, viewport, point size, and less-than
depth test as the visible pass. The picking shader writes
`gl_VertexIndex + 1`; zero therefore means no point. The pass runs only when a
request exists and does not affect idle rendering or benchmark timing.

Qt logical cursor coordinates are converted to render-target physical pixels.
QRhi asynchronously reads a small cursor-centered rectangle. The closest
covered pixel to the rectangle center supplies a practical selection tolerance;
every nonzero candidate is still an actually visible, depth-tested point.

The returned ID maps to the CPU representation used to create that vertex:

- imported clouds read the corresponding entry in
  `LoadedPointCloud::points`;
- synthetic clouds use a new deterministic `generatePoint(index)` operation,
  and chunk generation delegates to that operation.

Both paths decode the unsigned 16-bit position exactly as the visible vertex
shader does.

## Asynchronous Request State

Readback never blocks the UI or render thread. The viewport allows one request
in flight and one pending request:

- wheel events at the pending cursor position accumulate their deltas;
- a newer cursor position replaces the pending position while retaining the
  accumulated wheel intent;
- a double-click request replaces a pending wheel request and takes priority;
  and
- repeated double-clicks retain only the latest request.

Each submitted request captures the camera revision and projection state used
by its picking pass. Any camera mutation increments the revision. A result
whose revision no longer matches is discarded and the latest unresolved
intent is resubmitted against the current view. This prevents a delayed GPU
result from moving the camera toward a point selected in an obsolete view.

When a current result completes:

- a wheel hit applies the accumulated dolly to the decoded point;
- a wheel miss applies the camera-forward fallback;
- a double-click hit replaces the orbit pivot; and
- a double-click miss leaves the pivot unchanged.

Applying navigation increments the camera revision and schedules a redraw.

## Components and Boundaries

`NavigationCamera` contains only camera mathematics and is tested without Qt or
Metal.

`NavigationInputState` tracks held movement keys, modifiers, frame delta, and
pick-request coalescing. It contains no QRhi resources and can be tested
deterministically.

`MetalPointPicker` owns the QRhi picking resources, records an on-demand pass,
and reports an asynchronous optional vertex ID. It does not decide how a pick
changes the camera.

`MetalRenderViewport` translates Qt events, coordinates the input state,
camera, and picker, constructs the projection, and applies resolved navigation
actions. The visible renderer continues to consume only the camera matrix.

This separation keeps camera behavior portable, isolates the backend-specific
picking mechanism, and avoids putting navigation details into `MainWindow`.

## Error and Edge Handling

- A zero-sized viewport defers picking and ignores pan scaling.
- A pick outside the render target is treated as a miss.
- Empty-space double-click does not change the pivot.
- A zero-length eye-to-target vector falls back to camera-forward.
- Non-finite input, camera, or projection values are rejected without updating
  camera state.
- Pick requests are cancelled during QRhi resource release and cannot call
  back into destroyed viewport state.
- Resizing invalidates in-flight projection state; unresolved intent is
  resubmitted after the new picking target is ready.
- Loading a new point cloud invalidates pending picks, resets the camera by
  framing the cloud, and increments the camera revision.

## Testing

Core camera tests cover:

- framing and initial orientation;
- unrestricted camera position and movement through the pivot;
- orbit preserving the selected pivot's screen-relative relationship;
- pan axes and distance-scaled sensitivity;
- forward, lateral, and world-vertical keyboard directions;
- normalized diagonal movement;
- `Shift` and `Option`/`Alt` speed factors;
- close-range movement below one quantization interval;
- finite adaptive near and far planes at close and distant positions; and
- rejection of invalid viewport dimensions and non-finite values.

Input-state tests cover:

- key press, release, auto-repeat, and focus-loss behavior;
- frame-delta capping;
- one-in-flight/one-pending request behavior;
- wheel-delta accumulation;
- double-click priority;
- stale camera-revision rejection and resubmission; and
- hit and miss action selection.

Synthetic point tests verify that `generatePoint(index)` and
`generatePointChunk(first, count)` remain byte-for-byte consistent.

A Metal integration test displays a cloud containing a known center point,
submits a center pick through the same picker API used by input handling, and
does not succeed until asynchronous readback returns that point's expected
vertex ID. It also verifies an empty-pixel miss. Existing renderer and
file-loading smoke tests remain required.

Manual acceptance verifies:

1. Wheel movement stays aligned to a visible point under the cursor.
2. Double-clicking an off-center point makes orbit retain that point.
3. Right-drag remains screen-relative at different orientations and distances.
4. `W/S/A/D/Q/E`, `Shift`, and `Option`/`Alt` behave as documented.
5. The camera can enter, traverse, and leave the point cloud.
6. A selected point remains visible at distances far below the old `1.25`
   limit.
7. `F`, new-cloud loading, resize, and focus loss leave navigation stable.
8. Idle frame metrics are unaffected because the pick pass is on demand.

## Deferred Work

- Persistent spatial selection or storing selected point IDs.
- Camera animation, inertia, collision, gravity, roll, or gamepad controls.
- Configurable key bindings and sensitivity UI.
- A navigation gizmo, pivot marker, or crosshair.
- Native Metal picking or compute-based selection.
- LOD-aware selection beyond the currently rendered active point prefix.
- Windows and Linux picker implementations.
