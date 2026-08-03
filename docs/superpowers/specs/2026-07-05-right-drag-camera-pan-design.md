# Right-Drag Camera Pan Design

## Purpose

Add conventional camera panning to the point-cloud viewport. Holding the right
mouse button and dragging moves the cloud with the cursor in screen space.

## Behavior

- Right-drag right moves the cloud right.
- Right-drag up moves the cloud up.
- Panning follows the camera's current screen-right and screen-up axes, so it
  remains intuitive after orbiting.
- Pan sensitivity scales with camera distance and logical viewport height. A drag
  therefore covers the same approximate fraction of the visible world at every
  zoom level and window size.
- Left-drag orbit and wheel zoom remain unchanged.
- Pressing both buttons does not combine operations; the most recently pressed
  supported button selects the active drag operation until it is released.

## Architecture

`OrbitCamera` owns the world-space focus point in addition to yaw, pitch, and
distance. Its `panFromDrag()` method converts pixel delta into a world-space
focus-point translation using:

- the current yaw and pitch;
- a 60-degree vertical field of view;
- the current camera distance; and
- the viewport's logical-pixel height, matching Qt mouse-event coordinates.

The translation moves the camera focus opposite the requested on-screen cloud
motion. The eye position remains the focus point plus the existing orbit
offset, so orbiting after a pan continues around the new focus point.

`MetalRenderViewport` tracks one drag mode (`none`, `orbit`, or `pan`). Right
press selects pan, mouse movement supplies the pixel delta and render-target
height to `OrbitCamera::panFromDrag()`, and right release ends panning. The view
matrix uses the camera's focus-point getters for both eye and look-at target.

The camera math remains in the API-independent core so later Windows and Linux
viewports share identical interaction semantics.

## Testing

Core tests verify:

- default-orientation right/up drags move the focus point in the direction that
  makes the cloud follow the cursor;
- the pan axes rotate with camera yaw and pitch;
- sensitivity increases proportionally with camera distance; and
- zero viewport height leaves the focus point unchanged.

The existing renderer smoke test verifies that adding the focus point does not
break Metal rendering or startup.
