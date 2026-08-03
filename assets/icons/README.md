# Point Cloud Inspector application icons

Survey Lens is the production icon for Point Cloud Inspector. It combines a literal
magnifying glass with a simplified point-cloud terrain sample and uses the
application's Strata palette.

## Layout

- `pcinspector-256.png` is embedded into the executable as the runtime Qt
  window icon.
- `platform/macos/PointCloudInspector.icns` is copied into the application bundle and
  named by `CFBundleIconFile`. The adjacent `.iconset` retains all ten standard
  and Retina source representations.
- `platform/windows/PointCloudInspector.ico` contains 15 representations from 16
  through 256 pixels. `PointCloudInspector.rc` embeds it as the executable icon.
- `platform/linux/hicolor/` follows the Freedesktop icon-theme layout with
  fixed 16, 24, 32, 48, 64, 96, 128, 256, and 512 pixel PNGs plus a scalable
  SVG. CMake installs these files and `pcinspector.desktop` into the standard
  data directories.
- `source/` contains the full-detail generated master and the deterministic
  optical-size SVG masters.

## Optical sizing

PNGs from 16 through 96 pixels are rendered from five hand-authored optical
masters rather than reduced from the large bitmap:

| Output size | Optical master | Detail policy |
|---:|---:|---|
| 16 | 16 | Two terrain facets, three oversized nodes, maximum ring/handle weight |
| 20, 24 | 24 | Two terrain facets, three oversized nodes |
| 30, 32 | 32 | Three terrain facets, five nodes |
| 36, 40, 48 | 48 | Three terrain facets, seven nodes |
| 60, 64, 72, 80, 96 | 96 | Three terrain facets, seven nodes, wider spacing |

The platform bundles deliberately use these tailored small representations.
The 128 pixel and larger assets retain the full-detail master artwork.

## Regeneration

Render an optical master directly to its target dimensions with librsvg. For
example:

```sh
rsvg-convert -w 24 -h 24 source/survey-lens-24.svg -o pcinspector-24.png
```

Compile the macOS iconset with `iconutil -c icns`. Build the Windows ICO from
the exact-size PNGs, preserving each image as a separate representation; do
not resize the large master to recreate the small assets.
