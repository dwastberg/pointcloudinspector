# Point Cloud Inspector

Point Cloud Inspector is a C++23 and Qt 6 desktop application for viewing
LAS, LAZ, COPC, and EPT point clouds through PDAL and Qt's cross-platform QRhi
renderer.

## License

Point Cloud Inspector's source code and original project assets are licensed
under the [GNU General Public License v3.0 or later](LICENSE)
(`GPL-3.0-or-later`). Copyright © 2026 Dag Wästberg.

Third-party components retain their own licenses. Catch2 is referenced as a
submodule, and the vendored Earcut files include their license alongside the
source.

## Vector overlays

The viewer also imports OGR-readable local vector files supported by the
compiled GDAL drivers. Point, line, and polygon sublayers are compiled off the
GUI thread and appear as independently styled Scene layers.

Vector support is deliberately a **planar overlay** feature: every imported
layer is rendered at one configured Z plane. It is not terrain draping and it
does not reproject CRS coordinates. Layers whose reported X/Y extent is
disjoint from the current scene are retained hidden, with an explicit **Show
anyway** action, so Fit Scene remains stable until the user opts in.

Use **File → Import Vector Layer…** (`Ctrl+Shift+V`) or the **Vector…**
command-bar action. Multi-sublayer sources open a non-blocking chooser with
type and feature-count summaries. Select the resulting Scene layer to edit
fill, stroke, markers, opacity, and placement in the Inspector. If the source
plane sits outside the point-cloud height range, use **Place above scene**,
**Match scene floor**, or **Always on top**; import progress and retry actions
remain available in Tasks.

COPC and EPT use hierarchical, demand-loaded rendering with bounded decoded
CPU and GPU caches. Ordinary local LAS/LAZ above one million source points is
indexed once into the application's persistent local page cache and then uses
the same bounded hierarchy-residency path. When the exact leaf working set of
all visible local page sources fits both configured caches, the viewer warms
and pins that set, then atomically settles on every leaf point. Camera motion
does not apply LOD or adaptive thinning in that state. Smaller LAS/LAZ remains
a progressive, point-capped in-memory path. See
the archived design records for the
[hierarchical point source](docs/archive/HIERARCHICAL_POINT_SOURCE_DESIGN.md)
and [local point page store](docs/archive/LOCAL_POINT_PAGE_STORE_DESIGN.md) for
the original residency and cache contracts.

## Prerequisites

- CMake 3.24 or newer.
- A supported C++23 compiler and standard library. Configuration verifies both
  the compiler floor and `std::expected` support:

  | Compiler | Minimum version | CI representative |
  |---|---:|---|
  | GCC | 12 | Ubuntu 24.04 GCC |
  | Clang | 16 with libc++ 16+ or a compatible libstdc++ | Ubuntu 24.04 Clang |
  | AppleClang | 15 with the matching SDK libc++ | macOS 15 AppleClang |
  | MSVC | 19.33 (Visual Studio 2022 17.3) | Windows Server 2022 MSVC |
- Qt 6.7 or newer with Concurrent, Core, Gui, GuiPrivate, ShaderTools, and
  Widgets. Tests additionally require Qt Test.
- PDAL 2.10 or newer, including the `pdalcpp` CMake target.
- GDAL 3.4 or newer. Development and automated testing currently use GDAL
  3.13; the declared minimum-version CI/deployment lane remains a release gate.
- The vendored Catch2 submodule when `BUILD_TESTING=ON`.

The renderer uses Qt's private QRhi headers. The Qt runtime must therefore come
from the same Qt build as the headers and libraries used at compile time.

Initialize dependencies after cloning:

```sh
git submodule update --init --recursive
```

If CMake cannot locate Qt or PDAL, set `Qt6_ROOT` and `PDAL_ROOT` in your local
environment or in an ignored `CMakeUserPresets.json`. When cross-compiling, use
the platform's CMake toolchain file rather than hard-coding search paths in the
project.

## Configure, build, and test

The shared presets deliberately do not force a generator, so they work with
Ninja, Unix Makefiles, Xcode, and Visual Studio defaults:

```sh
cmake --preset development
cmake --build --preset development --parallel
ctest --preset development
```

For a warnings-as-errors CI-style build:

```sh
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset ci
```

For memory, undefined-behavior, and concurrency checks, use the dedicated
non-GPU sanitizer presets:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel
ctest --preset asan-ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan
```

Both presets run the complete non-GPU suite and gate CI. Sanitizers require
AppleClang, Clang, or GCC and are mutually exclusive. The recorded baseline is in
[`docs/sanitizer-baseline.md`](docs/sanitizer-baseline.md).

When `clang-format` is installed, CMake also exposes project-only formatting
targets (vendored and generated sources are excluded):

```sh
cmake --build --preset development --target format
cmake --build --preset development --target format-check
```

The static-analysis lane uses the focused project configuration in `.clang-tidy`.
Every enabled diagnostic fails the build and the lane gates CI:

```sh
cmake --preset clang-tidy
cmake --build --preset clang-tidy --parallel
```

It requires `clang-tidy` on `PATH`, under `LLVM_ROOT/bin`, or in a standard
Homebrew LLVM prefix. The triage history and exclusions are recorded in
[`docs/clang-tidy-baseline.md`](docs/clang-tidy-baseline.md).

For the platform-primary real-GPU lane with its validation layer enabled:

```sh
cmake --preset native-gpu
cmake --build --preset native-gpu --parallel
ctest --preset native-gpu
```

That preset selects Metal on Apple platforms, D3D12 on Windows, and Vulkan
elsewhere. Optional/manual fallback coverage uses the same build with
`-DPCINSPECTOR_GPU_TEST_GRAPHICS_API=opengl` (or another supported API).

For an optimized application without tests or developer tools:

```sh
cmake --preset release
cmake --build --preset release --parallel
```

Equivalent manual configuration remains supported:

```sh
cmake -S . -B build/local -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/local --parallel
ctest --test-dir build/local --output-on-failure
```

## Build options

| Option | Default | Purpose |
|---|---:|---|
| `BUILD_TESTING` | `ON` | Build and register automated tests. |
| `PCINSPECTOR_BUILD_TOOLS` | `OFF` | Build profiling tools such as `pci_load_bench`. |
| `PCINSPECTOR_COLOR_MAP_DIRECTORY` | `assets/colormaps` | Directory recursively scanned for `.cpt` color maps to embed. |
| `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI` | `OFF` | Compile developer profiling, residency, renderer telemetry, and qualification controls into the application UI. |
| `PCINSPECTOR_ENABLE_GPU_TESTS` | `OFF` | Enable tests that need a native GPU and display backend. |
| `PCINSPECTOR_GPU_TEST_GRAPHICS_API` | Platform primary | Select `auto`, `metal`, `vulkan`, `d3d11`, `d3d12`, or `opengl` for the native GPU lane. |
| `PCINSPECTOR_GPU_TEST_VALIDATION` | `ON` | Enable backend validation in the native GPU lane. |
| `PCINSPECTOR_SANITIZER` | `none` | Select `none`, `address-undefined`, or `thread` instrumentation on supported compilers. Prefer the sanitizer presets. |
| `PCINSPECTOR_WARNINGS_AS_ERRORS` | `OFF` | Promote project compiler warnings to errors. |
| `PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES` | `OFF` | Run Qt's deployment helper during installation. |

GPU tests are intentionally opt-in because they cannot run reliably in a
headless environment. The remaining unit, component, Qt, and offscreen UI tests
run through CTest.

## Runtime controls

```sh
pcinspector --graphics-api auto --cpu-cache-mb auto --gpu-cache-mb 512 --max-points 10000000 cloud.copc.laz
```

- `--graphics-api` accepts `auto`, `metal`, `vulkan`, `d3d11`, `d3d12`, or
  `opengl`. `auto` leaves selection to Qt. Explicit choices are fixed before
  the viewport is shown; a failed backend reports how to retry with `auto`.
- `--gpu-validation` enables the backend debug/validation layer in builds made
  with `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI=ON`.
- `--cpu-cache-mb` is the total resident point-payload budget shared by flat
  previews and hierarchical decoded pages. It defaults to `auto`, which uses
  effective installed RAM, current headroom, the GPU/decode allowances, and a
  system reserve. The ceiling is refreshed while the application runs; an
  explicit positive MiB value remains available as a hard user ceiling.
- `--gpu-cache-mb` is the global GPU point-buffer residency budget.
- `--max-points` defaults to 10 million. For small retained LAS/LAZ it is the
  in-memory sampling cap; for paged LAS/LAZ, COPC, and EPT it limits exposed
  hierarchy depth and detail.
- `--qualification-report <path>` writes the Release H native load/render
  report after the settled display-ready frame. Add `--qualification-exit` to
  close the process after the atomic JSON file is committed. These options are
  available only in diagnostic builds and are intended for repeatable
  validation runs with one or more command-line files.

The default Layers dock shows loaded layers, essential metadata and color
controls. Source activity appears only for active, failed, or cancelled work;
successful history is represented by the layer list. Its context menu can
prioritize, cancel, retry, or dismiss a job. A loaded layer's context menu
opens source-wide point-cloud statistics covering XYZ bounds and distributions,
bounding-box density and nominal spacing, RGB/intensity ranges, classification
and return distributions, and sampled spatial-outlier estimates. The source
scan runs in the background and is cancelled when its dialog closes. Batch
progress is weighted by measured work, later previews do not wait behind a
slower earlier file, and final layer order still matches the input order.

The main toolbar exposes two scene-wide rendering controls. Eye-Dome Lighting
is enabled by default and can be toggled without reloading the scene. Point
size is selectable from 1–8 framebuffer pixels and defaults to 2 pixels;
picking uses the same point size and expands its candidate cone accordingly.

The Tools menu and viewport rail provide Navigate (`N`) and Measure (`M`).
Measure previews the nearest visible rendered point within 10 logical pixels,
then snaps each click to that point. The first two clicks show a transient
3D, horizontal, and absolute vertical distance in source coordinate units. Left-drag
continues to orbit, right-drag pans, the wheel zooms, and Escape clears the
current measurement.

Continuous X, Y, Z, and intensity coloring has an `Auto` range plus editable
minimum and maximum values for each layer. Automatic X/Y/Z uses the combined
source bounds of every loaded layer, including hidden layers, so equal world
coordinates keep equal colors throughout the scene. Automatic intensity uses
the exact source-wide range once an unthinned import or local-page indexing
completes. Before that range is known—or when a flat source is intentionally
sampled—it uses the stable LAS domain `[0, 65535]`, never the currently
resident or retained sample. Selecting a different color source returns that
source to its automatic range; changing only the color map preserves a manual
range.

Color maps are data-driven. Built-ins remain in `PointColorMapCatalog.cpp`;
every `.cpt` file below `assets/colormaps` is discovered by CMake, embedded as
a Qt resource, parsed once during startup, and added to the scalar color-map
catalog before the UI or renderer reads it. Adding, removing, or editing a CPT
there therefore only requires a rebuild. An optional
`# PCINSPECTOR_NAME = Display name` comment controls its UI label; otherwise
the filename is used. Regular RGB, grayscale, HSV, and CMYK intervals are
supported, including discontinuities. Invalid or unsupported maps report their
resource path and source line at startup while built-in maps remain available.
The source folder can be replaced at configure time with
`PCINSPECTOR_COLOR_MAP_DIRECTORY`.

Renderer startup samples the combined catalog into a padded RGBA8 lookup
atlas, so neither new CPT maps nor runtime scalar ranges require shader or QRhi
pipeline edits. The point shaders contain no Viridis, Turbo, classification,
or return-number implementation. The application and native-GPU tests also
share one shader inventory through `cmake/PciShaders.cmake`. Review and
retain each third-party CPT's license and attribution before distributing it.

With `PCINSPECTOR_ENABLE_DIAGNOSTIC_UI=ON`, the Layers dock additionally shows
per-layer local-index state and a Residency panel. The diagnostic status bar
distinguishes the visible layers' immutable source points,
retained flat sample, decoded CPU residency, GPU residency, adaptive request,
frame selection, and actually submitted points. It also reports visible and
culled blocks; draw calls and submitted frames; uniform capacity, growth, and
update operations; uploaded and pending bytes with QRhi batch counts; and CPU
time spent snapshotting, selecting, uploading, and recording commands. The
existing decoded-cache, process-memory, source, and decode-admission counters
remain available in the same line. Backend diagnostics show the requested and
selected API. Pick diagnostics show candidate versus visible block and point
counts, making broad-phase effectiveness directly observable. `Full warming`
means an exact local leaf set has passed CPU/GPU admission and is loading;
`Full resident` means leaf-only rendering is active; `Full LOD` means the
working set does not fit or the source cannot enumerate a finite leaf set.

Diagnostic metrics are sampled at a low frequency during active rendering and once more
when the viewport settles, so the final counters remain observable while the
GPU sleeps. Stable flat scenes label a reused frame plan, and a completed load
reports dispatch-to-first-points latency. Diagnostic Debug builds also place
QRhi debug markers around point uploads, the main point pass, uniform updates,
and picking for backend capture tools. The `profiling` and `native-gpu`
presets enable this compile-time option; the `development` and `release`
presets keep the default UI clean.

In a diagnostic build, set `PCI_PROFILE_RENDERING=1` to write low-frequency,
settled-frame render profiles to the process log. Each record includes backend,
total frame time, snapshot/selection/upload/command CPU phases,
requested/selected/submitted points, draw calls, pick candidate/input work,
current/peak GPU point-buffer bytes, the GPU budget, cumulative GPU evictions,
decoded CPU residency, and the full-detail state. The later color-control work
added one portable LUT atlas to remove map definitions from GLSL and make
runtime ranges possible.
Eye-dome lighting and bounded 1–8-pixel hardware point sizing are implemented
and covered by native-GPU image tests. Circular splats, density-aware sizing,
and MSAA remain future options that require representative comparisons on the
required GPU lanes.

Completed flat LAS/LAZ scenes render all retained points that fit the GPU
residency budget while settled. During genuinely slow interaction the adaptive
budget can reduce submitted detail, but it distributes that detail across a
deterministic set of stable block identities instead of replacing whole regions
as their camera-distance ordering changes. Mouse orbit and pan are timed as
continuous interaction and the viewport returns to event-driven sleep when the
button is released.

### Local LAS/LAZ page cache

The first open of an ordinary LAS/LAZ with more than one million points scans
the source once. A coarse spatial preview is published after at most 65,536
points, so rendering can begin while bounded Morton sorting and parent-page
construction continue. Before commit, that early sample is replaced in the
cache manifest by a bounded root sampled from the completed hierarchy, avoiding
a permanently source-order-biased preview on later opens. The source file is
never modified. A completed index is atomically committed below Qt's
platform-native application cache directory in the `point-pages` folder;
reopening an unchanged source validates its identity and representative root
page without scanning the full file.

The loading overlay treats the first rendered root as a preview, not as load
completion. When all visible finite local sources fit the configured CPU and
GPU budgets, it remains open through measured leaf decoding and GPU upload and
closes only after the atomic full-detail frame is active. The full-detail phase
reports decoded and uploaded points separately, so a warm cache no longer
appears complete several seconds before the cloud becomes visible.

Leaf pages contain at most 32,768 points even when coordinates are highly
clustered. Page payloads and manifests are checksummed, interrupted builds do
not become cache hits, active entries are protected by process leases, and old
entries are pruned to a 20 GiB default disk allowance. Construction also checks
its estimated temporary storage against that allowance and currently available
filesystem space. Cache identity includes the canonical source path, size,
modification time, header metadata, sampled source bytes, and page-format
revision, so a changed source or incompatible format creates a new entry.

After a page store is committed, it also exposes its sorted finite leaf set
and exact point/byte working set. The renderer admits full residency only when
every visible layer is complete, no point was omitted by `--max-points`, each
layer fits its CPU allocation, and the aggregate decoded and GPU payloads fit
with one-eighth headroom. During warming the existing hierarchy remains
drawable. The switch to leaf-only rendering happens only after every leaf is
decoded and uploaded, so a capacity-qualified cloud does not change density as
the camera moves. If a layer or budget change invalidates admission, the pins
are released and normal bounded LOD resumes.

## Stress and benchmark qualification

The deterministic stress suite covers a decoded working set greater than 20×
its CPU cache, rapid request cancellation, and more than 10× cumulative native
GPU residency churn through alternating hierarchical layers. Run the CPU lane
on any build host and the GPU lane on a real device:

```sh
ctest --test-dir build/development -L stress --output-on-failure
build/gpu/tests/pcinspector_gpu_tests "[stress]"
```

The GPU test deliberately constrains residency to one coarse parent plus eight
children. It verifies complete parent-to-child transitions, CPU and GPU peak
bounds, cache eviction and source reload counts, validation-layer cleanliness,
and that rendering sleeps after every churn cycle. GPU tests remain opt-in;
use the `native-gpu` preset to create `build/gpu` first.

An optimized tools build records decode throughput, cache/source/decode
pressure, cancellation, and process-memory measurements from local files:

```sh
cmake --preset profiling
cmake --build --preset profiling \
    --target pci_load_bench pci_residency_bench pci_multifile_bench \
             pci_qualification_diff --parallel
build/profiling/tools/pci_load_bench --max-points 10000000 cloud.copc.laz
build/profiling/tools/pci_residency_bench \
    --cpu-cache-mb 64 \
    --stress-nodes 160 \
    --passes 2 \
    --minimum-working-set-multiple 10 \
    cloud.copc.laz
build/profiling/tools/pci_multifile_bench \
    --cpu-cache-mb 4096 \
    --cache-dir qualification/page-cache \
    --json qualification/local-files.json \
    first.laz second.laz additional-files.las
build/profiling/tools/pci_qualification_diff \
    qualification/baseline.json \
    qualification/candidate.json \
    qualification/tolerances.json
```

`pci_load_bench` measures sequential and bounded-parallel decode throughput.
`pci_residency_bench` discovers non-empty hierarchy nodes, walks them in both
directions, reloads evicted nodes, attempts an obsolete-query cancellation,
and samples RSS every 2 ms. It reports cache current/peak/budget bytes, the
largest decoded node, hit/miss/eviction counts, source query outcomes and
decoded volume, peak decoder allowance, cancellation latency, and baseline,
sampled-peak, final, and operating-system high-water RSS.

`pci_multifile_bench` uses the production batch controller, document-wide
cache, scheduler, and local-page adapter for every supplied LAS/LAZ file. It
records first-any/first-all preview, import/index and page-request latency,
index size/reuse, scheduler peaks, cache/decode cancellation, current/peak RSS,
and whether residency stayed bounded. It accepts an arbitrary file list; there
is no fixed source-count limit. Reproducible Release H measurements and scope
notes are archived in [`qualification/README.md`](qualification/README.md).

`pci_qualification_diff` first rejects reports whose schema, success status,
backend, validation mode, budgets, full-detail mode, or stable source metadata
do not match. It then applies the committed per-measurement tolerances and exits
with status 5 on regression. Exit 3 means malformed or unsupported input, and
exit 4 means the reports describe different workloads. Paths and timestamps
are deliberately ignored.

Each file produces one `RESIDENCY_RESULT` line for scripts. With
`--minimum-working-set-multiple 10`, the command exits with status 3 unless the
source is hierarchical, the discovered decoded working set is at least 10×
the CPU budget, current cache residency is within budget, and the cache's
transient insertion peak is within budget plus one largest node. Status 1 is a
load/runtime failure and status 2 is invalid input. Run this against a
representative local production file before claiming workload-qualified
out-of-core behavior. CTest generates both an eight-point command/format smoke
fixture and a deterministic 64,000-point local hierarchy whose 160-node working
set exceeds a 1 MiB cache by more than 10×. The latter exercises the real PDAL
adapter, eviction, reload, cancellation, and RSS sampler, but it is still too
small and regular to substitute for representative production data.

The page cache makes large ordinary local LAS/LAZ demand-loadable without
requiring source conversion. Native hierarchical formats remain useful when an
external workflow already produces them; local paging does not depend on COPC,
EPT, or HTTP.

## Install

```sh
cmake --install build/release --config Release --prefix dist
```

On macOS this installs an application bundle; on Windows and Linux it installs
the executable under the standard runtime directory. Installation deliberately
does not bundle runtime dependencies by default. A controlled packaging build
with a self-contained Qt and PDAL dependency graph can opt in to Qt's deployment
helper with `-DPCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES=ON`. Platform packaging
should additionally verify PDAL and its non-Qt runtime dependencies,
particularly on macOS and Windows.

The development macOS bundle identifier is currently the pre-release
placeholder `local.pointcloudinspector.pcinspector`. Replace it with the
permanent reverse-DNS identifier before signing or publishing the application.
