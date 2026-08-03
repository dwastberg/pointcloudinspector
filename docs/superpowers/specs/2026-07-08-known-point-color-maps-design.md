# Known Point Color Maps Design

## Purpose

Add first-pass point-cloud color mapping without changing the renderer's hot
path or sacrificing large-cloud performance. The viewer should continue to use
source RGB values when available, and it should also be able to color loaded
LAS/LAZ/COPC points by X, Y, Z, intensity, classification, return number, or
number of returns.

This pass intentionally supports only the known fields already represented by
the current import model. Arbitrary PDAL dimensions are deferred until the data
model and UI need to expose dynamic source dimensions.

The feature succeeds when:

- RGB remains the default for files that contain color;
- files without RGB default to a useful scalar mode, starting with Z;
- users can switch color source without reloading the point cloud;
- switching color source does not rebuild or re-upload the point vertex buffer;
- the existing picker and navigation behavior are unchanged; and
- all added per-point GPU data fits in the existing 16-byte `GpuPoint` layout.

## Scope

### Included

- Known color sources:
  - RGB;
  - X;
  - Y;
  - Z;
  - Intensity;
  - Classification;
  - Return number; and
  - Number of returns.
- A small renderer-facing color-mode contract.
- UI selection for available color sources.
- GPU-side color mapping driven by uniforms.
- Packing full known LAS attributes into the currently unused `GpuPoint` lane.
- Tests for packing, source availability, default mode selection, and renderer
  color-mode API behavior.

### Deferred

- Arbitrary numeric PDAL dimensions.
- Per-file user-defined color ramps.
- Histogram equalization or percentile clipping.
- Filter-by-attribute controls.
- Legend rendering.
- Persistent user preferences.
- CPU recoloring/export of colorized points.

## Current Constraints

`LoadedPointCloud` currently contains two aligned arrays:

- `points`: compact `GpuPoint` records used by the renderer; and
- `attributes`: full known point attributes preserved for future filtering and
  coloring.

The Metal display pipeline renders one immutable point vertex buffer with a
16-byte stride. The picker pipeline uses the same position data and derives the
picked point from `gl_VertexIndex`; it does not need display color.

The current `GpuPoint` layout is:

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

The `padding` field is already uploaded to the GPU as part of each point but
is not read by the shader. This is the cheapest place to carry the known
properties needed by the first color-map pass.

## Data Layout

Rename the unused `GpuPoint::padding` field to a purpose-specific packed field,
for example `packedProperties`, while keeping `sizeof(GpuPoint) == 16`.

Use the packed lanes as follows:

- `attributes` bits 0..7: classification, preserving the existing contract.
- `attributes` bits 8..15: coarse intensity, preserving the existing contract.
- `packedProperties` bits 0..15: full 16-bit intensity.
- `packedProperties` bits 16..23: return number.
- `packedProperties` bits 24..31: number of returns.

X, Y, and Z color modes use the existing quantized coordinate fields directly.
RGB mode uses the existing `rgba` field.

This layout adds no per-point memory and no additional upload bandwidth. It
also keeps the picker vertex input compatible because the picker reads only the
first position attribute.

## Color-Mode Model

Introduce a small common color-mode contract near the renderer interface:

```cpp
enum class PointColorSource {
    Rgb,
    X,
    Y,
    Z,
    Intensity,
    Classification,
    ReturnNumber,
    NumberOfReturns,
};

struct PointColorMode {
    PointColorSource source = PointColorSource::Rgb;
};
```

The initial pass needs only source selection. A later pass can extend this with
palette, scalar range, clipping, inversion, and categorical styling without
changing how the selected source is represented.

`RenderViewport` gains:

```cpp
virtual void setColorMode(PointColorMode mode) = 0;
virtual std::vector<PointColorSource> availableColorSources() const = 0;
```

The renderer validates requested modes against the loaded cloud metadata. If a
source is unavailable, it keeps the current mode and reports a clear failure
through the same application-level path used for invalid renderer operations.
X, Y, and Z are always available for non-empty point clouds.

## Default Source Selection

When a cloud is loaded:

1. Select RGB if `metadata.hasColor` is true.
2. Otherwise select Z.

Z is the best non-RGB default for point-cloud inspection because it usually
shows terrain, structure height, and vertical layering immediately. X and Y are
still always available from the selector.

Synthetic clouds keep their existing RGB default.

## Shader and GPU Flow

Extend the display vertex input to read the packed properties at byte offset
12 as one unsigned integer attribute, or as an equivalent integer-compatible
QRhi attribute format if required by Qt's exact `QRhiVertexInputAttribute`
format names.

Add a small uniform block for color mapping. It should contain:

- selected source;
- scalar minimum;
- scalar maximum;
- flags for categorical versus scalar mapping; and
- padding for stable uniform alignment.

For the first pass, scalar ranges are fixed and cheap:

- X/Y/Z: 0..65535 from quantized coordinates;
- Intensity: 0..65535;
- Return number: 0..255 using the packed 8-bit value; and
- Number of returns: 0..255 using the packed 8-bit value.

Classification uses a categorical palette. RGB bypasses scalar mapping and
passes through `rgba`.

The fragment shader remains simple: it outputs the interpolated point color.
For point-list rendering there is no meaningful interpolation across vertices.

Color-source changes update only the color uniform and schedule a repaint. They
must not dirty point resources, rebuild the immutable vertex buffer, or trigger
the loading overlay.

## Palette Strategy

Use hard-coded first-pass palettes in the shader or a tiny uniform block:

- scalar values: grayscale or a compact perceptual ramp;
- classification: fixed categorical colors for common LAS classes, with a
  fallback color for unknown classes; and
- RGB: source color unchanged.

The first implementation should prefer deterministic, readable colors over a
large configurable palette system. A palette texture or larger uniform palette
can be added later if visual quality becomes a priority.

## UI Flow

Add a color-source selector in the existing toolbar or menu area. It should be
populated from `availableColorSources()` after each load and disabled for
sources not present in the loaded file.

Recommended labels:

- RGB;
- X;
- Y;
- Z;
- Intensity;
- Classification;
- Return number;
- Number of returns.

The selector updates the renderer immediately. The status bar can remain
metrics-focused; it does not need to display the active color source in this
first pass.

## Performance Requirements

The display draw path remains:

- one immutable point vertex buffer;
- one point-list draw call;
- the same 16-byte vertex stride;
- no CPU-side recoloring on source changes;
- no point-buffer re-upload on source changes; and
- no changes to the picker render pass.

The only expected runtime cost is a small amount of extra vertex-shader math
and one additional integer attribute read from the existing vertex record. This
is acceptable for the proof-of-concept renderer and avoids the much larger cost
of CPU recoloring or sidecar GPU buffers.

## Error Handling

If the UI asks for an unavailable source, the renderer should reject the request
deterministically. The first-pass behavior should be:

- keep the previous color mode;
- log or report a clear message that the source is unavailable; and
- continue rendering.

Loading should not fail just because optional color properties are absent.
Unavailable optional properties simply do not appear in the selector.

## Testing and Acceptance

Add focused tests for:

- `GpuPoint` remains 16 bytes after renaming/reusing the packed property lane;
- full intensity, return number, and number of returns are packed correctly
  during PDAL import;
- loaded clouds preserve `attributes.size() == points.size()`;
- default color source is RGB when color is available and Z otherwise;
- `availableColorSources()` includes X/Y/Z for loaded clouds and includes only
  optional fields reported by metadata;
- `setColorMode()` accepts available sources without marking point resources
  dirty or requiring re-upload; and
- missing shader resources still fail cleanly.

Manual acceptance:

1. Load an RGB LAS/LAZ file and verify RGB is selected by default.
2. Switch to Z and verify the cloud recolors immediately without reloading.
3. Switch to intensity or classification on a file with those dimensions.
4. Verify picking, mouse navigation, and keyboard navigation still work.
5. Confirm switching color sources does not show the loading overlay.

## Future Extension Path

When arbitrary PDAL dimensions become necessary, add a sidecar attribute buffer
or a selected scalar buffer rather than expanding `GpuPoint`. That future path
can reuse the same `PointColorSource` UI concept by adding dynamic dimension
descriptors and per-dimension scalar ranges.

The first pass deliberately keeps the known LAS fields in the existing point
record because that provides immediate functionality with no increase in memory
bandwidth or upload time.
