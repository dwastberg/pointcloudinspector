# Z-Up Navigation Design

## Goal

Use positive Z as the application's default world-up axis while preserving point
coordinates as native XYZ values.

## Coordinate Convention

- Positive Z is world up.
- The default camera position is `(0, -4, 0)`.
- The default camera looks along positive Y toward the origin.
- Positive X remains camera-right in the default view.
- Q moves along negative Z and E moves along positive Z.

## Implementation

Define the world-up vector once in the navigation core and use it for camera
basis calculation, orbit yaw, pitch constraints, and keyboard elevation.
`NavigationCamera::frameScene()` restores the Z-up default position and
orientation.

Point-cloud data is not transformed. The point and picking vertex shaders
continue interpreting their inputs as XYZ coordinates, so visible rendering and
GPU picking use the same coordinate system.

## Behavior

Left-drag orbit rotates around the Z axis for yaw and preserves Z as vertical.
Right-drag pan remains aligned with the camera's screen plane. F restores the
default view from negative Y. Existing wheel-to-point dolly and free movement
behavior remain unchanged.

## Testing

Core camera tests verify the default position, forward vector, up vector,
framing, orbit direction, and screen-plane panning under Z-up. Renderer contract
tests verify F restores the Z-up view and Q/E map to the Z world axis. The full
test suite and Metal smoke test must continue to pass.

## Scope

This change does not add configurable coordinate systems, dataset metadata
interpretation, axis widgets, or shader-side axis swapping.
