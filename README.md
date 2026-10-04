# Point Cloud Inspector

Point Cloud Inspector is a desktop application for viewing and inspecting point
clouds alongside raster imagery, elevation data, and vector overlays on Linux,
Windows, and macOS.

## Features

- Explore large point clouds with GPU rendering and on-demand loading.
- Color points by RGB, coordinates, intensity, classification, or return
  information, with built-in color maps and LAS classification filters.
- Inspect layer metadata and point-cloud statistics, adjust point size, and
  improve depth perception with eye-dome lighting (**Depth enhancement**).
- Navigate in 3D, switch between perspective and orthographic views, or use a
  locked top-down **2D Map View**.
- Measure 3D, horizontal, and vertical distances between visible points.
- Display raster imagery and styled vector overlays alongside point clouds.
- View eligible elevation rasters as 3D surfaces with vertical exaggeration,
  surface shading, and adjustable color ramps.
- Transfer a raster's displayed colors to a point cloud and restore the original
  colors when needed.

## Getting started

1. Choose **File → Open Files…**, drag files onto the window, or pass filenames
   on the command line. You can open several files of different types together.
2. Select a layer to inspect its properties and adjust its appearance. Use the
   layer controls to show, hide, isolate, or remove layers.
3. Press **F** to fit all visible layers. Left-drag to orbit, right-drag to pan,
   and scroll to zoom toward the cursor. Double-click to set the orbit pivot.
4. Press **M** to measure: hover to snap to a visible point, then click two
   endpoints. **Escape** clears the measurement; **N** returns to navigation.

In 3D, **W/A/S/D** move the camera and **Q/E** move down/up. Hold **Shift** for
faster movement or **Alt** for fine movement. Press **7** for a top-down view.
In **2D Map View**, left-drag pans and the camera stays top-down.
Open **Help → Viewport Controls** for the controls reference.

### Supported data

| Data | Supported sources |
|---|---|
| Point clouds | LAS, LAZ, COPC (`.copc.laz`), and EPT (`ept.json`) |
| Vectors | GeoPackage, GeoJSON/JSON, Shapefile, KML, GML, FlatGeobuf, and DXF |
| Rasters | TIFF/GeoTIFF/COG, VRT, GTI tile catalogs, IMG, JPEG 2000, PNG, and JPEG |

Format support depends on the PDAL and GDAL/OGR drivers available in the build.
GeoPackage files open as vector data, including `.gti.gpkg`; raster catalog
routing recognizes `.gti`, `.gti.fgb`, and `.gti.shp`.

Layers use their source coordinates without automatic reprojection. Use matching
coordinate systems for aligned overlays and raster colorization. Vector points,
lines, and polygons appear as styled planar overlays and do not follow terrain.
Measurements snap to rendered point-cloud points, not raster or vector layers.

### Raster imagery and terrain

Select a raster layer to change opacity and elevation offset. Continuous
single-band rasters also offer a display range and color ramp. Eligible elevation
rasters expose **Terrain rendering → Mode → Surface (true elevation)**, with
controls for vertical exaggeration and surface shading when supported by the
graphics device. The raster remains flat while its exact elevation range is
being analyzed.

Elevation values are used without implicit unit conversion. For meaningful
relief, use a projected coordinate system with horizontal and vertical units that
agree. Imagery is displayed on a plane; it is not draped over other layers.

Raster tiles load as you navigate. When suitable source overviews are missing,
the application uses an automatic low-resolution preview. Adding overviews to
the source improves zoomed-out display and navigation.

### Color a point cloud from a raster

Open both layers, select the point cloud, and choose **Layer → Colorize from
Raster…**. Select the raster and review the memory and temporary-disk estimate
before starting. The operation uses the raster's displayed colors; only opaque
pixels change points, and unmatched points retain their source colors.
Use **Revert to Source Colors** to restore the original colors.

## Performance and disk storage

Open **Settings** to adjust point and raster memory budgets. Automatic point CPU
memory budgeting is enabled by default. Point buffers, raster tiles, and GDAL's
own raster cache have separate budgets; these are not a cap on total application
memory. Raster worker-count changes take effect after restarting the application.

LAS/LAZ files loaded through the persistent page cache, along with native COPC
and EPT hierarchies, retain all source points regardless of the maximum-points
setting. Their decoded pages and GPU buffers load on demand within the configured
budgets. Reusable disk caches speed up later imports.

Under **Settings → Disk Storage**, **Refresh** rescans estimated usage and
**Open Folder** opens a storage location. **Clean unused files** removes unused
caches and working files in the background, protecting active files, source
datasets, and settings. Removed caches are rebuilt when needed.

Cleanup takes effect immediately; cancelling Settings does not undo it. For
**Clean legacy files…**, close other app instances and review the confirmation,
since older storage cannot always identify its owner.

## Command line

```sh
pcinspector survey.copc.laz imagery.tif boundaries.gpkg
pcinspector "tiles/*.laz"
```

Pass one or more filenames to open them together. You can mix supported point
clouds, rasters, and vectors, and use wildcard patterns (`*`, `?`, and `[set]`) in
filenames.

| Option | Purpose |
|---|---|
| `-h`, `--help` | Show command-line help. |
| `-v`, `--version` | Show the application version. |

## Building from source

To build the application yourself, you need:

- CMake 3.24 or newer.
- A C++23 compiler: GCC 12+, Clang 16+, AppleClang 15+, or MSVC 19.33+, with a
  standard library supporting `std::expected`, `std::jthread`, and `std::stop_token`.
- Qt 6.7 or newer, including Widgets, private GUI headers, and ShaderTools.
- PDAL 2.10 or newer and GDAL 3.9 or newer. Raster catalogs require GDAL's GTI
  driver and a suitable index driver, such as FlatGeobuf or ESRI Shapefile.

```sh
git clone --recurse-submodules https://github.com/dwastberg/pointcloudinspector.git
cd pointcloudinspector
cmake --preset release
cmake --build --preset release --parallel
cmake --install build/release --config Release --prefix dist
```

The release preset builds the optimized application without tests or developer
tools. The local installation places the executable in `dist/bin` on Linux and
Windows, or `Point Cloud Inspector.app` in `dist` on macOS. It does not bundle
runtime dependencies; keep the required libraries available. The application
must run with the same Qt build it was compiled against.

If dependencies are not found, supply their installation prefixes through
`CMAKE_PREFIX_PATH`, `Qt6_ROOT`, or `PDAL_ROOT`. Build requirements and presets
are defined in [CMakeLists.txt](CMakeLists.txt) and
[CMakePresets.json](CMakePresets.json).

## Architecture overview

The C++23 application uses Qt Widgets for its desktop interface. A session
coordinates scene metadata, layer controls, and background operations. Separate
point, raster, and scene runtimes manage streamed data and caches; frame planning
and rendering use Qt's QRhi graphics abstraction. PDAL handles point-cloud input,
GDAL/OGR handle raster and vector input, and local storage adapters manage caches
and temporary files.

The current module layout is listed in [src/CMakeLists.txt](src/CMakeLists.txt),
with dependency boundaries in
[cmake/PciArchitecture.cmake](cmake/PciArchitecture.cmake).

## License

Point Cloud Inspector is licensed under the
[GNU General Public License v3.0 or later](LICENSE).
