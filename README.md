# Point Cloud Inspector

Point Cloud Inspector is a cross-platform desktop viewer for point-cloud data.
It is built with C++23, Qt 6, PDAL, and GDAL.

## Features

- Open LAS, LAZ, COPC, and EPT point clouds, including multiple files in one
  scene.
- Navigate large datasets with GPU-accelerated rendering and bounded CPU and
  GPU caches.
- Color points by stored RGB, coordinates, intensity, classification, or
  return information using the included CPT color maps.
- Inspect layer metadata and point-cloud statistics, filter LAS
  classifications, and adjust point size and eye-dome lighting.
- Measure 3D, horizontal, and vertical distances between rendered points.
- Import GDAL/OGR-readable point, line, and polygon layers as styled planar
  overlays. Vector data is not reprojected or draped over the point cloud.

## Building

### Requirements

- CMake 3.24 or newer
- A C++23 compiler: GCC 12+, Clang 16+, AppleClang 15+, or MSVC 19.33+
- Qt 6.7 or newer, including its private GUI headers and ShaderTools
- PDAL 2.10 or newer
- GDAL 3.4 or newer

Clone the repository and initialize its submodules:

```sh
git clone https://github.com/dwastberg/pointcloudinspector.git
cd pointcloudinspector
git submodule update --init --recursive
```

Configure and build a development version, then run the tests:

```sh
cmake --preset development
cmake --build --preset development --parallel
ctest --preset development
```

Build an optimized version without the test suite:

```sh
cmake --preset release
cmake --build --preset release --parallel
```

Optionally install it to a local directory:

```sh
cmake --install build/release --config Release --prefix dist
```

If CMake cannot find a dependency, provide its installation prefix through
`CMAKE_PREFIX_PATH`, `Qt6_ROOT`, or `PDAL_ROOT`.

## Command line

```text
pcinspector [options] [files...]
```

`files` may be LAS, LAZ, COPC, or EPT `ept.json` sources. Multiple files and
wildcard patterns (`*`, `?`, and `[set]`) are supported.

| Argument | Description | Default |
|---|---|---:|
| `-h`, `--help` | Show command-line help. | |
| `--help-all` | Show application and Qt command-line options. | |
| `-v`, `--version` | Show the application version. | |
| `-p`, `--points <count>` | Create a synthetic point cube with the requested number of points. | off |
| `--max-points <count>` | Maximum number of source points exposed by a loaded point cloud. | `10000000` |
| `--cpu-cache-mb <MiB\|auto>` | CPU budget shared by retained previews and decoded hierarchy pages. | `auto` |
| `--gpu-cache-mb <MiB>` | GPU point-buffer residency budget. | `512` |
| `--graphics-api <api>` | Select `auto`, `metal`, `vulkan`, `d3d11`, `d3d12`, or `opengl`. | `auto` |
| `--smoke-test` | Exit after three rendered frames. | off |

Example:

```sh
pcinspector --cpu-cache-mb auto --gpu-cache-mb 1024 \
  --max-points 50000000 survey.copc.laz
```

Builds configured with `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI=ON` also provide:

| Argument | Description |
|---|---|
| `--gpu-validation` | Enable the graphics backend debug or validation layer. |
| `--qualification-report <path>` | Write load and rendering qualification metrics as JSON after the display is ready. |
| `--qualification-exit` | Exit after writing the qualification report. Requires `--qualification-report`. |

## License

Point Cloud Inspector is licensed under the
[GNU General Public License v3.0 or later](LICENSE).
