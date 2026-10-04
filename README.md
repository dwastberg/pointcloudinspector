# Point Cloud Inspector

Point Cloud Inspector is a cross-platform desktop viewer for point-cloud data.
It is built with C++23, Qt 6, PDAL, and GDAL.

## Features

- Open mixed batches of point clouds, vector layers, and raster layers from a
  file dialog, the command line, or by dragging files onto the window.
- Navigate large datasets with GPU-accelerated rendering and bounded CPU and
  GPU caches.
- Color points by stored RGB, coordinates, intensity, classification, or
  return information using the included CPT color maps.
- Inspect layer metadata and point-cloud statistics, filter LAS
  classifications, and adjust point size and eye-dome lighting.
- Measure 3D, horizontal, and vertical distances between rendered points.
- Import GDAL/OGR-readable point, line, and polygon layers as styled planar
  overlays. Vector data is not reprojected or draped over the point cloud.
- Import GDAL-readable raster imagery and terrain as georeferenced planar
  layers with adjustable opacity and elevation. Rasters are not reprojected,
  not draped, not lit by eye-dome lighting, and not pickable. Display quality
  depends on the overviews the dataset provides; add them with `gdaladdo`.

## Disk storage cleanup

Open **Settings → Disk Storage** to see estimated file sizes for point-cloud
caches, temporary working files, and legacy storage. **Refresh** rescans usage;
**Open Folder** shows each storage location.

**Clean unused files** runs in the background and keeps files used by open
datasets, imports, colorization, or another app instance. Source datasets and
settings are preserved. Cleanup takes effect immediately; cancelling Settings
does not restore deleted files. Clearing a reusable cache makes the next import
rebuild it.

**Clean legacy files…** handles older storage that cannot always identify its
owner. Close other app instances and check the confirmation before cleaning it.
Known active files are still protected. Errors and incomplete scans are shown
in the dialog; reported sizes estimate file contents, not physical disk blocks.

New point caches use `point-pages-v3` so older app versions cannot bypass the
cleanup ownership protocol. Existing `point-pages-v2` caches remain visible as
legacy storage; the first import in the new version rebuilds its cache.

## Building

### Requirements

- CMake 3.24 or newer
- clang-format 22.1.8 for formatting checks (CMake rejects a different detected version)
- A C++23 compiler: GCC 12+, Clang 16+, AppleClang 15+, or MSVC 19.33+
- Qt 6.7 or newer, including its private GUI headers and ShaderTools
- PDAL 2.10 or newer
- GDAL 3.9 or newer, built with the GTI raster tile-index driver and at least
  one index format (GeoPackage, FlatGeobuf, or ESRI Shapefile)

A version floor is not a driver guarantee, so check what a given build actually
has rather than inferring it from the version:

```sh
pcinspector --gdal-capabilities
```

It prints the GDAL version and each raster driver's availability, and exits
non-zero when catalog import is unavailable. The packaging jobs run it against
the built application for exactly that reason: a package ships its own GDAL,
not the developer machine's.

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

## Architecture and validation

See the [ownership and extension guide](docs/refactor-architecture.md) for module
boundaries, operation lifetimes, cache eviction and admitted colorization scratch.
The [refactor progress ledger](PCI_REFACTOR_PROGRESS.md) records executed checks
and remaining qualification limitations.

```sh
cmake --build --preset development --target header-self-containment format-check
cmake -DBUILD_DIR=build/development -DCTEST_CONFIGURATION=Debug -P tests/cmake/CheckQualificationTestRegistration.cmake
ctest --preset development -L architecture --output-on-failure --no-tests=error
ctest --preset development -R '^qualification_diff_' --output-on-failure --no-tests=error
cmake -DPCINSPECTOR_LOCAL_CHECK_JOBS=4 -P cmake/RunFullChecks.cmake
```

Native GPU tests use the `native-gpu` preset on a host with a working native
graphics/display environment. Remote Linux/Windows/macOS CI execution is separate
from locally passing checks.

CI and packaging batch ordinary Catch2 cases by executable and run three CTest
workers (two for ASan/UBSan and TSan). Console failures still name the case;
case-level JUnit reports are written to `build/<preset>/Testing/Reports/` and
uploaded with CTest diagnostics. Batch CTest names and labels identify the
executable, so `ctest --preset ci -R pcinspector_document_tests` selects that
suite. Use `ctest --preset ci --rerun-failed` to rerun failed suites, or run a
test executable directly with a case name or Catch2 tag, for example:

```sh
build/ci/tests/pcinspector_document_tests '[snapshot]'
```

The development preset retains individual case discovery and tag labels. Set
`-DPCINSPECTOR_BATCH_TESTS=OFF` when configuring another preset to use the same
granular CTest filtering. GPU and raster scale cases always use separate
processes; long stress cases also run serially. Sanitizer default builds omit
duplicate header self-containment compilation; the explicit
`header-self-containment` target remains available.

Long stress tests are **off by default in CI and packaging**. Ordinary CI keeps
smaller multi-tile raster bake/cancellation tests and the inexpensive catalog
and benchmark smoke tests. Full-scale catalog churn, sparse BigTIFF streaming,
million-point bakes and the large residency benchmark carry the `long-stress`
label. Enable **Run workflow → run_long_stress_tests** in GitHub Actions to run
them on all CI configurations; Linux must still pass before Windows starts,
and Windows must pass before macOS starts. The qualification checks run once
within each configuration's CTest suite.

Development and `RunFullChecks.cmake` retain long stress coverage. To run just
that coverage locally, or enable it for a sanitizer build:

```sh
cmake --preset development
cmake --build --preset development --parallel 3
ctest --preset long-stress

cmake --preset asan-ubsan -DPCINSPECTOR_ENABLE_LONG_STRESS_TESTS=ON
cmake --build --preset asan-ubsan --parallel 3
ctest --preset asan-ubsan -L '^long-stress$'
```

With `PCINSPECTOR_ENABLE_LONG_STRESS_TESTS=OFF`, the long cases are not registered
with CTest and the large benchmark fixture is not generated. Running a test
executable directly bypasses CTest selection; use `'~[long-stress]'` to exclude
long cases when invoking the raster stress executable yourself.

### CLion formatting

Use external **clang-format 22.1.8** and the repository `.clang-format`. That version
is pinned in CI dependency environments and is available for Linux, Windows and
macOS in the [conda-forge package files](https://anaconda.org/channels/conda-forge/packages/clang-format/files).
Set `PCINSPECTOR_CLANG_FORMAT_EXECUTABLE` when the pinned executable is not first
on PATH; on the current macOS development host it is
`/opt/homebrew/bin/clang-format`.

In CLion Settings, enable ClangFormat under Editor → Code Style → C/C++, select
the external executable under the ClangFormat tool settings, and use the project
configuration file. Under Tools → Actions on Save, enable Reformat code for C/C++
and select the whole file rather than changed lines. Verify the selected binary
with `clang-format --version`; the bundled IDE formatter may be a different
version. Reload the CMake project after module moves. Run `format-check` to verify
that IDE formatting agrees with CI.

## Command line

```text
pcinspector [options] [files...]
```

`files` may mix supported point-cloud, vector, and raster sources. Multiple
files and wildcard patterns (`*`, `?`, and `[set]`) are supported. GeoPackage
sources are opened as vector data in the initial unified workflow.

| Argument | Description | Default |
|---|---|---:|
| `-h`, `--help` | Show command-line help. | |
| `--help-all` | Show application and Qt command-line options. | |
| `-v`, `--version` | Show the application version. | |
| `-p`, `--points <count>` | Create a synthetic point cube with the requested number of points. | off |
| `--max-points <count>` | Maximum points retained for a non-paged point-cloud source. Paged and hierarchical sources retain all points. | `10000000` |
| `--cpu-cache-mb <MiB\|auto>` | CPU budget shared by retained previews and decoded hierarchy pages. | `auto` |
| `--gpu-cache-mb <MiB>` | GPU point-buffer residency budget. | `512` |
| `--raster-cpu-cache-mb <MiB>` | Decoded raster tile budget, separate from the point caches. | `256` |
| `--raster-gpu-cache-mb <MiB>` | Raster tile texture budget on the graphics device. | `256` |
| `--gdal-cache-mb <MiB>` | GDAL block cache, the third allocator holding raster pixels. | `128` |
| `--raster-workers <count>` | Raster tile read threads, 1-8. Applies at startup. | `2` |
| `--graphics-api <api>` | Select `auto`, `metal`, `vulkan`, `d3d11`, `d3d12`, or `opengl`. | `auto` |
| `--gdal-capabilities` | Print this build's GDAL version and raster driver availability, then exit. | |
| `--smoke-test` | Exit after three rendered frames. | off |

Example:

```sh
pcinspector --cpu-cache-mb auto --gpu-cache-mb 1024 \
  survey.copc.laz
```

LAS/LAZ files routed through the persistent page cache, and native COPC/EPT
hierarchies, preserve their complete source regardless of `--max-points`.
Their decoded CPU and GPU working sets remain bounded by the cache options.

Builds configured with `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI=ON` also provide:

| Argument | Description |
|---|---|
| `--gpu-validation` | Enable the graphics backend debug or validation layer. |
| `--qualification-report <path>` | Write load and rendering qualification metrics as JSON after the display is ready. |
| `--qualification-exit` | Exit after writing the qualification report. Requires `--qualification-report`. |

## License

Point Cloud Inspector is licensed under the
[GNU General Public License v3.0 or later](LICENSE).
