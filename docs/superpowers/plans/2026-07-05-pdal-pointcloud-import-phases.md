# PDAL Point-Cloud Import Phases Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add production-oriented point-cloud file loading in five independently usable, fully tested phases without coupling PDAL to the render loop.

**Architecture:** PDAL is isolated in an import library that produces renderer-owned data. Phase 1 loads bounded local files into one GPU buffer; Phase 2 introduces in-memory chunks and per-chunk transforms; Phase 3 persists those chunks in a versioned cache with LOD; Phase 4 adds direct asynchronous COPC preview; Phase 5 adds bounded GPU residency, visibility selection, and optional attributes. Every phase ends with a release gate and leaves the application useful if later phases are never implemented.

**Tech Stack:** C++23, CMake 3.24+, Qt 6 Core/Concurrent/Gui/Widgets/ShaderTools/Test, QRhi/Metal, PDAL 2.10.x, CTest.

---

## Scope and fixed decisions

- macOS/Metal remains the only application target during these phases. Import, format, and scheduling libraries must remain platform-neutral.
- Phase 1 supports local `.las`, `.laz`, and `.copc.laz` files. Remote COPC starts in Phase 4.
- `--points` and synthetic generation remain available as a benchmark and fallback path.
- File decoding never runs on the GUI or render thread.
- Tests generate tiny deterministic LAS, LAZ, and COPC fixtures during test setup. Tests never require network access.
- PDAL is an import dependency only. No renderer header may include a PDAL header.
- A phase is complete only when its release gate passes from a clean build directory.
- Performance tests report measurements and enforce structural budgets such as maximum resident bytes. They do not assert machine-specific frame rates.
- The `.pci` cache introduced in Phase 3 is disposable and regenerable. It is not a long-term interchange format.

## Stable dependency direction

```text
app
 ├── import_async ── import_pdal ── PDAL
 ├── dataset_runtime ── pci_format
 └── renderer ── pointcloud_core

pointcloud_core
 ├── metadata and bounds
 ├── GpuPoint
 ├── PointChunk
 └── dataset/provider interfaces
```

`pointcloud_core` must not depend on Qt, PDAL, or QRhi. `pci_format` may depend on Qt Core for JSON, but not on PDAL or QRhi.

## Test fixture policy

Create `tests/fixtures/PdalFixtureFactory.{h,cpp}` in Phase 1. It writes this logical dataset through PDAL:

```cpp
inline constexpr std::array<FixturePoint, 8> fixturePoints{{
    {1000.0, 2000.0, 10.0, 65535,     0,     0, 100, 2},
    {1010.0, 2000.0, 10.0,     0, 65535,     0, 200, 2},
    {1000.0, 2010.0, 10.0,     0,     0, 65535, 300, 5},
    {1010.0, 2010.0, 10.0, 65535, 65535,     0, 400, 5},
    {1000.0, 2000.0, 20.0, 65535,     0, 65535, 500, 6},
    {1010.0, 2000.0, 20.0,     0, 65535, 65535, 600, 6},
    {1000.0, 2010.0, 20.0, 32768, 32768, 32768, 700, 1},
    {1010.0, 2010.0, 20.0, 65535, 65535, 65535, 800, 1},
}};
```

Generate `fixture.las`, compressed `fixture.laz`, and `fixture.copc.laz`. Every integration test compares against the logical points above rather than comparing one PDAL output file to another.

---

# Phase 1: Local Files to One GPU Buffer

## Deliverable

The user can open a local LAS, LAZ, or COPC file. The application inspects it, streams and deterministically limits points on a worker thread, maps the result to the existing 16-byte `GpuPoint`, replaces the synthetic GPU buffer, frames the camera, and reports progress or an actionable failure.

The entire loaded cloud uses one center and one uniform scale. This preserves aspect ratio and is intentionally replaced by per-chunk transforms in Phase 2.

## Phase 1 file map

- Create `src/pointcloud/Bounds3d.h`: validated double-precision bounds.
- Create `src/pointcloud/SourcePoint.h`: normalized double-precision importer record.
- Create `src/pointcloud/PointCloudMetadata.h`: source path, driver, CRS, dimensions, point count, bounds.
- Create `src/pointcloud/LoadedPointCloud.h`: immutable Phase 1 CPU result.
- Create `src/import/PointCloudImport.h`: PDAL-free import request/result/error interface.
- Create `src/import/pdal/PdalSourceInspector.{h,cpp}`: reader inference and metadata extraction.
- Create `src/import/pdal/PdalPointCloudLoader.{h,cpp}`: bounded streaming and point mapping.
- Create `src/import/PointCloudLoadController.{h,cpp}`: QtConcurrent execution, cancellation, and GUI-thread callbacks.
- Create `tests/pdal_import_tests.cpp`: fixture-backed import integration tests.
- Create `tests/load_controller_tests.cpp`: asynchronous state/cancellation tests.
- Create `tests/fixtures/PdalFixtureFactory.{h,cpp}`: deterministic local fixtures.
- Modify `src/renderer/RenderViewport.h`: accept immutable loaded data.
- Modify `src/renderer/metal/MetalRenderViewport.{h,cpp}`: replace GPU data safely.
- Modify `src/app/MainWindow.{h,cpp}`: Open action, progress, and errors.
- Modify `src/core/AppOptions.{h,cpp}` and `main.cpp`: optional positional input file and `--max-points`.
- Modify `CMakeLists.txt`: PDAL, Concurrent, Test, import targets, and tests.

### Task 1.1: Add the PDAL dependency and fixture target

- [ ] Add a configure-time PDAL requirement and isolated import target:

```cmake
find_package(PDAL 2.10 REQUIRED CONFIG)

add_library(pcinspector_pdal_dependency INTERFACE)
target_include_directories(pcinspector_pdal_dependency
    INTERFACE ${PDAL_INCLUDE_DIRS})
target_link_directories(pcinspector_pdal_dependency
    INTERFACE ${PDAL_LIBRARY_DIRS})
target_link_libraries(pcinspector_pdal_dependency
    INTERFACE ${PDAL_LIBRARIES})
```

- [ ] Configure to prove the dependency is found:

```bash
cmake -S . -B build-phase1 -DBUILD_TESTING=ON
```

Expected: configuration succeeds and prints a PDAL 2.10.x package location.

- [ ] Add `PdalFixtureFactory` and a `pdal_fixture_tests` target that creates all three fixture formats in a temporary directory.

- [ ] Run the fixture test:

```bash
cmake --build build-phase1 --target pdal_fixture_tests
ctest --test-dir build-phase1 -R pdal_fixture --output-on-failure
```

Expected: PASS; all three files exist and PDAL reports eight points.

- [ ] Commit:

```bash
git add CMakeLists.txt tests/fixtures tests/pdal_fixture_tests.cpp
git commit -m "test: add deterministic PDAL fixtures"
```

### Task 1.2: Define PDAL-free point-cloud data contracts

- [ ] Add failing contract tests to `tests/pointcloud_contract_tests.cpp` for invalid bounds, center, maximum extent, and immutable loaded data.

- [ ] Add the contracts:

```cpp
namespace pci {

struct Bounds3d {
    std::array<double, 3> minimum{};
    std::array<double, 3> maximum{};

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::array<double, 3> center() const noexcept;
    [[nodiscard]] double maximumExtent() const noexcept;
};

struct PointCloudMetadata {
    std::filesystem::path sourcePath;
    std::string sourceDriver;
    std::string spatialReferenceWkt;
    std::uint64_t sourcePointCount = 0;
    Bounds3d sourceBounds;
    std::vector<std::string> dimensions;
    bool hasColor = false;
    bool hasIntensity = false;
    bool hasClassification = false;
};

struct SourcePoint {
    std::array<double, 3> position{};
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t intensity = 0;
    std::uint8_t classification = 0;
    std::uint64_t sourceOrdinal = 0;
};

struct LoadedPointCloud {
    PointCloudMetadata metadata;
    std::vector<GpuPoint> points;
    std::array<double, 3> decodeCenter{};
    double decodeExtent = 1.0;
};

using LoadedPointCloudPtr = std::shared_ptr<const LoadedPointCloud>;

}
```

- [ ] Run:

```bash
cmake --build build-phase1 --target pointcloud_contract_tests
ctest --test-dir build-phase1 -R pointcloud_contract --output-on-failure
```

Expected: PASS, including degenerate single-point bounds.

- [ ] Commit:

```bash
git add CMakeLists.txt src/pointcloud tests/pointcloud_contract_tests.cpp
git commit -m "feat: define loaded point-cloud contracts"
```

### Task 1.3: Implement source inspection

- [ ] Write failing fixture-backed tests asserting:

```cpp
CHECK(metadata.sourcePointCount == 8);
CHECK(metadata.sourceBounds.minimum == std::array{1000.0, 2000.0, 10.0});
CHECK(metadata.sourceBounds.maximum == std::array{1010.0, 2010.0, 20.0});
CHECK(metadata.hasColor);
CHECK(metadata.hasIntensity);
CHECK(metadata.hasClassification);
CHECK(!metadata.sourceDriver.empty());
```

- [ ] Implement this public API:

```cpp
class PdalSourceInspector {
public:
    [[nodiscard]] PointCloudMetadata
    inspect(const std::filesystem::path &sourcePath) const;
};
```

Add `pcinspector_import_pdal` from `PdalSourceInspector.cpp` and `PdalPointCloudLoader.cpp`; link it publicly to `pcinspector_pointcloud` and privately to `pcinspector_pdal_dependency`. Use `StageFactory::inferReaderDriver()`, `Stage::preview()`, and prepared layout metadata. Reject an unknown reader, missing XYZ, zero points, non-finite bounds, or an unreadable file with an error containing both the path and PDAL driver.

- [ ] Run:

```bash
cmake --build build-phase1 --target pdal_import_tests
ctest --test-dir build-phase1 -R pdal_import --output-on-failure
```

Expected: LAS, LAZ, and COPC inspection cases pass; malformed and unsupported paths fail with stable error categories.

- [ ] Commit:

```bash
git add src/import/pdal/PdalSourceInspector.* tests/pdal_import_tests.cpp
git commit -m "feat: inspect point-cloud sources with PDAL"
```

### Task 1.4: Implement bounded point streaming and mapping

- [ ] Define the import interface in `src/import/PointCloudImport.h`:

```cpp
struct PointCloudImportRequest {
    std::filesystem::path sourcePath;
    std::uint64_t maximumPoints = 10'000'000;
    std::stop_token stopToken;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
};

class PointCloudImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PdalPointCloudLoader {
public:
    [[nodiscard]] LoadedPointCloudPtr
    load(const PointCloudImportRequest &request) const;
};
```

- [ ] Add failing tests for:

  - identical mapped points from LAS, LAZ, and COPC;
  - eight output points when `maximumPoints >= 8`;
  - four deterministic output points when `maximumPoints == 4`;
  - equal-axis normalization using the source maximum extent;
  - 16-bit RGB mapping to 8-bit RGBA;
  - white fallback when RGB is absent;
  - progress beginning at zero and ending at the processed source count;
  - cancellation producing a distinct cancelled error.

- [ ] Implement with a streamable PDAL terminal callback or `StreamPointTable`; do not call `execute(PointTable&)` on the whole source. Calculate a deterministic source stride from inspected point count and `maximumPoints`. Quantize all three axes with one `decodeExtent` so geometry is not stretched:

```cpp
q = std::clamp(
    std::lround(((coordinate - center) / extent + 0.5) * 65535.0),
    0L, 65535L);
```

- [ ] Run the import tests under a 64 MiB process-memory fixture containing at least one million generated points. Record peak importer-owned point bytes and assert they are bounded by `maximumPoints * sizeof(GpuPoint)` plus a fixed 8 MiB working allowance.

- [ ] Commit:

```bash
git add src/import/PointCloudImport.h src/import/pdal/PdalPointCloudLoader.* tests/pdal_import_tests.cpp
git commit -m "feat: stream PDAL points into GPU format"
```

### Task 1.5: Replace renderer data without recreating the window

- [ ] Extend the renderer contract:

```cpp
class RenderViewport {
public:
    // Existing members remain.
    virtual void setPointCloud(LoadedPointCloudPtr cloud) = 0;
};
```

- [ ] Add a renderer contract test that calls `setPointCloud()` before QRhi initialization and verifies the next smoke frame reports the loaded point count.

- [ ] Refactor `MetalRenderViewport` so `setPointCloud()` stores the immutable result, marks point resources dirty, and schedules `update()`. At the next `initialize()`/`render()` boundary, create a replacement immutable vertex buffer, upload it, then release the old buffer. Never mutate QRhi resources from the loading worker.

- [ ] Keep synthetic construction by wrapping generated points in a `LoadedPointCloud`.

- [ ] Run:

```bash
cmake --build build-phase1
ctest --test-dir build-phase1 -R 'renderer_contract|renderer_smoke' --output-on-failure
```

Expected: both tests pass; smoke rendering uses the loaded count.

- [ ] Commit:

```bash
git add src/renderer src/core/SyntheticPointCloud.* tests/renderer_contract_tests.cpp
git commit -m "feat: replace point-cloud data in Metal viewport"
```

### Task 1.6: Add asynchronous application loading

- [ ] Implement `PointCloudLoadController` as a `QObject` with one active job:

```cpp
class PointCloudLoadController final : public QObject {
    Q_OBJECT
public:
    explicit PointCloudLoadController(QObject *parent = nullptr);
    void load(PointCloudImportRequest request);
    void cancel();

signals:
    void progressChanged(quint64 processed, quint64 total);
    void loaded(pci::LoadedPointCloudPtr cloud);
    void failed(QString message);
    void cancelled();
};
```

Register `LoadedPointCloudPtr` as a Qt metatype. Use `QtConcurrent::run`, `QFutureWatcher`, and `std::stop_source`. Ignore completion from superseded jobs by monotonically increasing job ID.

- [ ] Add Qt tests using a fake blocking loader:

  - signals are delivered on the controller/GUI thread;
  - a second request cancels and supersedes the first;
  - destruction requests stop and waits without callbacks into a dead object;
  - progress is monotonic;
  - exceptions become `failed`, not process termination.

- [ ] Add `File > Open…`, `File > Cancel Loading`, a status-bar progress message, and a non-modal error dialog to `MainWindow`. On success call `viewport_->setPointCloud()`.

- [ ] Add an optional positional source path and `--max-points` to `main.cpp`; a provided path starts loading after the window is shown.

- [ ] Run:

```bash
cmake --build build-phase1
ctest --test-dir build-phase1 -R 'load_controller|app_options' --output-on-failure
```

Expected: all async and parsing tests pass with ThreadSanitizer in a separate debug configuration where available.

- [ ] Commit:

```bash
git add CMakeLists.txt src/import/PointCloudLoadController.* src/app main.cpp src/core/AppOptions.* tests
git commit -m "feat: load point clouds asynchronously from the application"
```

### Task 1.7: Phase 1 end-to-end gate

- [ ] Add CLI smoke cases:

```bash
pcinspector --smoke-test --max-points 8 fixture.las
pcinspector --smoke-test --max-points 8 fixture.laz
pcinspector --smoke-test --max-points 8 fixture.copc.laz
```

Expected: exit 0 after three rendered frames; log reports eight source and active points.

- [ ] Run the clean Phase 1 gate:

```bash
cmake -S . -B build-phase1-clean -DBUILD_TESTING=ON
cmake --build build-phase1-clean
ctest --test-dir build-phase1-clean --output-on-failure
```

Expected: 100% tests pass.

- [ ] Manually verify one real LAS/LAZ file: responsive window during load, correct aspect ratio/color, cancellation, camera controls, and no renderer recreation.

**Phase 1 exit criterion:** local files are genuinely usable through one bounded CPU result and one GPU buffer. No Phase 2 code is required for correctness.

---

# Phase 2: In-Memory Chunks and Per-Chunk Rendering

## Deliverable

Loaded datasets are split into immutable in-memory chunks with independent double-precision origins and scales. The renderer draws several chunks from a bounded GPU buffer set and can evict/re-upload chunks. There is no persistent `.pci` format and no LOD yet.

## Phase 2 file map

- Create `src/pointcloud/PointChunk.{h,cpp}`: chunk identity, transform, bounds, points.
- Create `src/pointcloud/ChunkBuilder.{h,cpp}`: deterministic fixed-count spatial chunks.
- Create `src/runtime/ChunkResidencyCache.{h,cpp}`: byte-budgeted CPU LRU.
- Create `src/runtime/DrawPlanner.{h,cpp}`: point-budget allocation across resident chunks.
- Create `src/renderer/ChunkRenderData.h`: renderer-facing immutable chunk collection.
- Create `tests/chunk_builder_tests.cpp`, `tests/residency_cache_tests.cpp`, and `tests/draw_planner_tests.cpp`.
- Modify `shaders/points.vert`: dynamic per-draw chunk transform.
- Modify `src/renderer/RenderViewport.h` and `src/renderer/metal/MetalRenderViewport.{h,cpp}`.
- Modify Phase 1 loader to emit chunks through `ChunkBuilder`.

### Task 2.1: Define and test chunk transforms

- [ ] Add these contracts:

```cpp
using ChunkId = std::uint64_t;
inline constexpr ChunkId invalidChunkId =
    std::numeric_limits<ChunkId>::max();

struct ChunkTransform {
    std::array<double, 3> origin{};
    std::array<double, 3> scale{};

    [[nodiscard]] std::array<double, 3>
    decode(const GpuPoint &point) const noexcept;
};

struct PointChunk {
    ChunkId id = 0;
    std::uint32_t level = 0;
    Bounds3d bounds;
    ChunkTransform transform;
    std::vector<GpuPoint> points;
};
```

- [ ] Test encode/decode round trips at minimum, midpoint, and maximum; enforce absolute error no greater than half the per-axis quantization step plus floating-point epsilon. Explicitly test zero-extent axes.

- [ ] Run:

```bash
cmake --build build-phase2 --target chunk_builder_tests
ctest --test-dir build-phase2 -R chunk_builder --output-on-failure
```

- [ ] Commit:

```bash
git add src/pointcloud/PointChunk.* src/pointcloud/ChunkBuilder.* tests/chunk_builder_tests.cpp CMakeLists.txt
git commit -m "feat: add quantized in-memory point chunks"
```

### Task 2.2: Build deterministic spatial chunks

- [ ] Implement:

```cpp
struct ChunkBuildOptions {
    std::uint32_t targetPointCount = 250'000;
};

class ChunkBuilder {
public:
    [[nodiscard]] std::vector<PointChunk>
    build(std::span<const SourcePoint> points,
          const Bounds3d &rootBounds,
          const ChunkBuildOptions &options) const;
};
```

Use a Morton key derived from normalized source coordinates, stable-sort by key and source ordinal, then split by `targetPointCount`. Derive stable chunk IDs from the chunk ordinal at level zero.

- [ ] Refactor the Phase 1 PDAL mapping loop to send each selected `SourcePoint` to a caller-supplied sink. Keep the Phase 1 sink that immediately produces globally quantized `GpuPoint` values; add a Phase 2 sink that retains at most `maximumPoints` source records and passes them to `ChunkBuilder`. This prevents re-quantizing the Phase 1 16-bit representation and losing precision.

- [ ] Test deterministic IDs and bytes across repeated runs, maximum points per chunk, complete point coverage, non-overlapping source ordinals, bounds containment, and degenerate clouds.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase2 -R chunk_builder --output-on-failure
git add src/pointcloud/ChunkBuilder.* tests/chunk_builder_tests.cpp
git commit -m "feat: partition loaded points into deterministic chunks"
```

### Task 2.3: Add bounded residency and draw planning

- [ ] Define:

```cpp
class ChunkResidencyCache {
public:
    explicit ChunkResidencyCache(std::uint64_t byteBudget);
    bool insert(std::shared_ptr<const PointChunk> chunk);
    [[nodiscard]] std::shared_ptr<const PointChunk> find(ChunkId id);
    void pin(ChunkId id);
    void unpin(ChunkId id);
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
};

struct DrawCommand {
    ChunkId chunkId;
    std::uint32_t firstPoint;
    std::uint32_t pointCount;
};

class DrawPlanner {
public:
    [[nodiscard]] std::vector<DrawCommand>
    plan(std::span<const ChunkId> orderedVisibleChunks,
         const ChunkResidencyCache &cache,
         std::uint64_t pointBudget) const;
};
```

- [ ] Test exact LRU eviction, pinned-entry protection, oversize rejection, byte accounting, deterministic draw order, partial final chunk, and a budget smaller than one chunk.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase2 -R 'residency_cache|draw_planner' --output-on-failure
git add src/runtime tests/residency_cache_tests.cpp tests/draw_planner_tests.cpp CMakeLists.txt
git commit -m "feat: add bounded chunk residency and draw planning"
```

### Task 2.4: Render multiple transformed chunks

- [ ] Extend `CameraUniform` with a dynamic-offset `ChunkUniform`:

```cpp
struct alignas(16) ChunkUniform {
    float originRelativeToDataset[4];
    float scale[4];
};
static_assert(sizeof(ChunkUniform) == 32);
```

Allocate a uniform-buffer slot per draw using `QRhi::ubufAlignment()`, bind it with `uniformBufferWithDynamicOffset`, and submit one `draw()` per planned chunk. Keep double world origins on CPU and convert them relative to a dataset origin before float upload.

- [ ] Update `points.vert` to decode:

```glsl
vec3 local = vec3(positionAttributes.xyz) / 65535.0;
vec3 position = chunk.originRelativeToDataset.xyz + local * chunk.scale.xyz;
```

- [ ] Add a two-chunk shader/renderer smoke test where both chunks use identical quantized points but different transforms. Verify both contribute to the draw count and QRhi validation reports no dynamic-offset error.

- [ ] Run and commit:

```bash
cmake --build build-phase2
ctest --test-dir build-phase2 -R 'renderer_contract|renderer_smoke' --output-on-failure
git add shaders src/renderer tests/renderer_contract_tests.cpp
git commit -m "feat: render transformed point chunks"
```

### Task 2.5: Phase 2 end-to-end gate

- [ ] Load each generated fixture through the Phase 1 UI and assert the status reports the expected chunk and point counts.
- [ ] Add a stress test with at least 100 chunks and a cache budget that fits ten; assert budget is never exceeded while cycling all chunk IDs twice.
- [ ] Run:

```bash
cmake -S . -B build-phase2-clean -DBUILD_TESTING=ON
cmake --build build-phase2-clean
ctest --test-dir build-phase2-clean --output-on-failure
```

Expected: 100% pass, including all Phase 1 tests.

**Phase 2 exit criterion:** multi-chunk rendering, byte-bounded residency, and per-chunk precision work entirely in memory. Persistent format and LOD are unnecessary for a usable application.

---

# Phase 3: Versioned `.pci` Cache, Disk Bucketing, and LOD

## Deliverable

Large sequential LAS/LAZ imports can be converted without retaining all points in memory. Conversion produces an atomic, versioned `.pci` directory cache. The application can reopen the cache, validate it, stream chunks, and render coarse-to-fine LOD without PDAL.

## Cache layout

```text
dataset.pci/
  manifest.json
  hierarchy.bin
  chunks/
    0000000000000000.bin
    0000000000000001.bin
```

`manifest.json` contains format version, source identity, CRS, source bounds, attribute mask, hierarchy count, and import options. Binary files use explicit little-endian encoding; C++ object memory is never written directly.

## Phase 3 file map

- Create `src/format/PciFormat.h`: magic, version, limits, disk DTOs.
- Create `src/format/PciBinaryIO.{h,cpp}`: checked little-endian reader/writer.
- Create `src/format/PciDatasetWriter.{h,cpp}`: temporary directory and atomic finalize.
- Create `src/format/PciDatasetReader.{h,cpp}`: validation and random chunk reads.
- Create `src/import/SpatialBucketBuilder.{h,cpp}`: bounded open-file bucket spilling.
- Create `src/import/PciImporter.{h,cpp}`: streaming PDAL-to-cache orchestration.
- Create `src/lod/LodBuilder.{h,cpp}`: deterministic voxel representatives.
- Create `src/runtime/PciDatasetProvider.{h,cpp}`: PDAL-free asynchronous cache access.
- Create `tests/pci_format_tests.cpp`, `tests/spatial_bucket_tests.cpp`, `tests/lod_builder_tests.cpp`, and `tests/pci_import_tests.cpp`.

### Task 3.1: Freeze format limits and binary primitives

- [ ] Define:

```cpp
inline constexpr std::array<char, 8> pciHierarchyMagic{
    'P','C','V','H','I','E','R','\0'};
inline constexpr std::array<char, 8> pciChunkMagic{
    'P','C','V','C','H','N','K','\0'};
inline constexpr std::uint32_t pciFormatVersion = 1;
inline constexpr std::uint64_t maximumChunkPayloadBytes = 256ULL << 20;
inline constexpr std::uint64_t maximumHierarchyNodes = 100'000'000;

struct SourceIdentity {
    std::uint64_t byteSize = 0;
    std::int64_t lastWriteTimeNanoseconds = 0;
    std::array<std::byte, 32> edgeHash{};
};

struct PciManifest {
    std::uint32_t formatVersion = pciFormatVersion;
    PointCloudMetadata metadata;
    SourceIdentity sourceIdentity;
    std::uint64_t importOptionsHash = 0;
    std::uint64_t hierarchyNodeCount = 0;
};

struct HierarchyNode {
    ChunkId id;
    ChunkId parentId;
    std::array<ChunkId, 8> childIds;
    std::uint32_t level;
    std::uint32_t pointCount;
    Bounds3d bounds;
    double geometricError;
};
```

- [ ] Test exact golden bytes for unsigned integers, doubles, magic, and version; truncated reads; wrong endianness marker; integer overflow; payload and hierarchy limits.

- [ ] Implement `PciBinaryReader` and `PciBinaryWriter` using byte arrays and explicit shifts/`std::bit_cast`, never packed structs.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R pci_format --output-on-failure
git add src/format/PciFormat.h src/format/PciBinaryIO.* tests/pci_format_tests.cpp CMakeLists.txt
git commit -m "feat: define versioned PCI binary primitives"
```

### Task 3.2: Write and validate atomic cache directories

- [ ] Implement writer lifecycle:

```cpp
class PciDatasetWriter {
public:
    void begin(const std::filesystem::path &finalPath,
               const PciManifest &manifest);
    void writeChunk(const PointChunk &chunk);
    void writeHierarchy(std::span<const HierarchyNode> nodes);
    void commit();
    ~PciDatasetWriter(); // removes uncommitted temporary directory
};
```

`begin()` rejects an existing final path. Rebuilds write a new sibling cache path derived from source identity and import options; the application switches providers only after `commit()` and may then remove the obsolete cache. This keeps the temporary-directory-to-new-directory rename atomic without pretending that replacing a non-empty directory is atomic on every supported filesystem.

- [ ] Implement reader lifecycle:

```cpp
class PciDatasetReader {
public:
    explicit PciDatasetReader(std::filesystem::path path);
    [[nodiscard]] const PciManifest &manifest() const noexcept;
    [[nodiscard]] std::span<const HierarchyNode> hierarchy() const noexcept;
    [[nodiscard]] PointChunk readChunk(ChunkId id) const;
};
```

- [ ] Test round-trip equality, missing chunk, duplicate ID, corrupted checksum, bad version, truncated hierarchy, cleanup after exception, rejection of an existing final path, and the final path remaining absent until successful commit.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R pci_format --output-on-failure
git add src/format/PciDatasetWriter.* src/format/PciDatasetReader.* tests/pci_format_tests.cpp
git commit -m "feat: read and atomically write PCI caches"
```

### Task 3.3: Add disk-backed spatial bucketing

- [ ] Implement:

```cpp
struct SpatialBucketOptions {
    std::uint32_t depth = 8;
    std::uint64_t memoryBudgetBytes = 256ULL << 20;
    std::size_t maximumOpenFiles = 64;
    std::filesystem::path temporaryDirectory;
};

class SpatialBucketBuilder {
public:
    void begin(Bounds3d rootBounds, SpatialBucketOptions options);
    void append(const SourcePoint &point);
    [[nodiscard]] std::vector<BucketRef> finalize();
};

struct BucketRef {
    std::uint64_t bucketId = 0;
    std::filesystem::path temporaryPath;
    std::uint64_t pointCount = 0;
    Bounds3d bounds;
};
```

`BucketRef` owns no file handle, and its paths are valid only until the importer completes or is cancelled.

Use deterministic Morton-prefix buckets, buffered appends, an LRU of open files, and fixed-width little-endian temporary records.

- [ ] Test boundary assignment, each point appearing exactly once, deterministic bucket bytes, open-file limit, memory budget, cleanup, cancellation, and a dataset larger than the memory budget.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R spatial_bucket --output-on-failure
git add src/import/SpatialBucketBuilder.* tests/spatial_bucket_tests.cpp
git commit -m "feat: bucket large imports with bounded disk spilling"
```

### Task 3.4: Build deterministic LOD

- [ ] Use the `HierarchyNode` contract fixed in Task 3.1 and implement bottom-up voxel representatives. Select the source point nearest each occupied voxel center; tie-break by child ID then source ordinal. This preserves real attributes and gives byte-stable results.

- [ ] Test one representative per occupied voxel, stable tie-breaking, hierarchy parent/child consistency, monotonic geometric error, root coverage, and identical output regardless of child input order.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R lod_builder --output-on-failure
git add src/lod tests/lod_builder_tests.cpp CMakeLists.txt
git commit -m "feat: generate deterministic point-cloud LOD"
```

### Task 3.5: Orchestrate PDAL-to-PCI conversion

- [ ] Implement `PciImporter` with these states:

```cpp
enum class ImportState {
    Inspecting,
    Bucketing,
    WritingLeaves,
    BuildingLod,
    Committing
};
```

It must stream the source once into buckets, finalize leaf chunks, build parent LOD, write the hierarchy, and atomically commit to a new cache path. Store source canonical path, byte size, last-write timestamp, and a SHA-256 hash of the first and last 1 MiB for cache invalidation.

- [ ] Test LAS and LAZ conversion against fixture points, cancellation in every state, no final cache after failure, source identity mismatch, deterministic repeated output, and importer-owned memory staying within its configured budget plus 16 MiB.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R pci_import --output-on-failure
git add src/import/PciImporter.* tests/pci_import_tests.cpp
git commit -m "feat: convert LAS and LAZ into PCI caches"
```

### Task 3.6: Load PCI without PDAL and select LOD

- [ ] Introduce a provider boundary:

```cpp
class PointCloudDatasetProvider {
public:
    struct Error {
        std::string message;
        bool cancelled = false;
    };
    using ChunkResult =
        std::expected<std::shared_ptr<const PointChunk>, Error>;
    using ChunkCallback = std::function<void(ChunkResult)>;

    virtual ~PointCloudDatasetProvider() = default;
    [[nodiscard]] virtual const PointCloudMetadata &metadata() const = 0;
    [[nodiscard]] virtual std::span<const HierarchyNode> hierarchy() const = 0;
    virtual void request(ChunkId id, ChunkCallback callback) = 0;
    virtual void cancel(ChunkId id) = 0;
};
```

- [ ] Implement `PciDatasetProvider` using a bounded worker pool and `PciDatasetReader`. Add a CPU selector with this interface:

```cpp
struct LodSelection {
    std::vector<ChunkId> desiredChunks;
    std::uint64_t estimatedPointCount = 0;
};

struct FrustumPlane {
    std::array<double, 3> normal;
    double offset;
};

struct CameraFrustum {
    std::array<FrustumPlane, 6> planes;
    std::array<double, 3> cameraPosition;
    double verticalFieldOfViewRadians;
};

class LodSelector {
public:
    [[nodiscard]] LodSelection select(
        std::span<const HierarchyNode> hierarchy,
        const CameraFrustum &frustum,
        double viewportHeightPixels,
        double maximumScreenSpaceError) const;
};
```

It uses frustum visibility and screen-space geometric error, returns ordered desired chunk IDs, and contains no QRhi code.

- [ ] Test request deduplication, cancellation, callback thread, corrupted chunk propagation, nearest/coarsest-first ordering, frustum rejection, and increasing detail as the error threshold decreases.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R 'pci_provider|lod_selector' --output-on-failure
git add src/runtime tests CMakeLists.txt
git commit -m "feat: stream PCI chunks with screen-space LOD"
```

### Task 3.7: Integrate cached datasets

- [ ] Add `.pci` directory opening and an explicit “Build Optimized Cache…” action. Never silently start a large conversion.
- [ ] Feed `LodSelector` results to the existing residency cache and draw planner. Show cache state, resident bytes, selected nodes, and active points in metrics.
- [ ] Add a smoke test that opens a generated cache after removing/renaming the original LAS, proving runtime loading has no PDAL/source dependency.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase3 -R 'renderer_smoke|pci_end_to_end' --output-on-failure
git add src/app src/renderer src/renderer/RenderMetrics.h tests CMakeLists.txt
git commit -m "feat: open and render optimized PCI caches"
```

### Task 3.8: Phase 3 gate

- [ ] Clean-build and run all tests:

```bash
cmake -S . -B build-phase3-clean -DBUILD_TESTING=ON
cmake --build build-phase3-clean
ctest --test-dir build-phase3-clean --output-on-failure
```

- [ ] Convert a real dataset larger than configured RAM budget, close the application, reopen only the `.pci`, and verify coarse-to-fine refinement, cancellation, bounded resident bytes, and source-cache invalidation.

**Phase 3 exit criterion:** LAS/LAZ production import and PDAL-free cached runtime loading are complete. COPC remains usable through Phase 1 full-file loading.

---

# Phase 4: Direct COPC Preview and Measured Cache Conversion

## Deliverable

Local and remote COPC sources open progressively without complete download or conversion. Source requests are asynchronous, cancellable, spatially bounded, and translated into the same `PointChunk`/provider interface used by `.pci`. Cache conversion remains explicit; the application switches to the cache only after atomic completion.

## Phase 4 file map

- Create `src/copc/CopcDatasetProvider.{h,cpp}`: hierarchy and bounded request adapter.
- Create `src/copc/CopcRequestScheduler.{h,cpp}`: priorities, deduplication, cancellation, concurrency.
- Create `src/copc/CopcBoundsTransform.{h,cpp}`: explicit source/project CRS query handling.
- Create `src/import/CopcCacheConverter.{h,cpp}`: COPC-to-PCI conversion.
- Create `src/benchmark/CopcPathBenchmark.cpp`: reproducible direct-versus-cache measurements.
- Create `tests/copc_provider_tests.cpp`, `tests/copc_scheduler_tests.cpp`, and `tests/copc_bounds_tests.cpp`.

### Task 4.1: Make COPC spatial requests CRS-safe

- [ ] Define:

```cpp
struct SpatialRequest {
    Bounds3d bounds;
    std::string boundsCrs;
    std::optional<double> resolution;
};

struct ProjectSpaceCrop {
    Bounds3d bounds;
    std::string crs;
};

struct SourceQuery {
    Bounds3d conservativeBounds;
    std::string sourceCrs;
    std::optional<ProjectSpaceCrop> postReprojectionCrop;
};
```

- [ ] Add an explicit PROJ dependency for this component. Use `proj_trans_bounds()` with 21-point boundary densification to transform project bounds into source CRS, expand by a numeric tolerance, query conservatively, then crop after reprojection. Use PDAL’s JSON bounds form with an explicit `crs` member.

- [ ] Test identity CRS, projected-to-projected, axis-order-sensitive geographic CRS, nonlinear conservative coverage, missing CRS, and transform failure.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase4 -R copc_bounds --output-on-failure
git add src/copc/CopcBoundsTransform.* tests/copc_bounds_tests.cpp CMakeLists.txt
git commit -m "feat: transform COPC spatial requests safely"
```

### Task 4.2: Schedule bounded COPC requests

- [ ] Implement a scheduler with:

```cpp
struct CopcRequestKey {
    ChunkId logicalChunk;
    std::uint32_t lodLevel;
};

using RequestHandle = std::uint64_t;

class CopcRequestScheduler {
public:
    explicit CopcRequestScheduler(std::size_t maximumConcurrentRequests);
    RequestHandle request(CopcRequestKey key, double priority,
                          std::function<PointChunk(std::stop_token)> work,
                          PointCloudDatasetProvider::ChunkCallback callback);
    void reprioritize(RequestHandle handle, double priority);
    void cancel(RequestHandle handle);
};
```

- [ ] Test concurrency limit, highest-priority-first order, duplicate key coalescing, independent subscribers, cancellation before/during execution, stale completion suppression, and clean shutdown.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase4 -R copc_scheduler --output-on-failure
git add src/copc/CopcRequestScheduler.* tests/copc_scheduler_tests.cpp
git commit -m "feat: schedule cancellable COPC requests"
```

### Task 4.3: Implement the COPC provider

- [ ] Implement `CopcDatasetProvider` behind `PointCloudDatasetProvider`. Opening reads metadata only and synthesizes a viewer-owned logical octree from source bounds; it does not claim to expose PDAL's private COPC hierarchy. Chunk requests configure `readers.copc` with explicit bounds and resolution, stream mapped points, and return ordinary `PointChunk` objects.

- [ ] Test local fixture open without full point materialization, coarse request containing fewer points than fine request, bounds containment, request deduplication, cancellation, and missing remote/network errors using an in-process HTTP range server serving the fixture.

- [ ] Assert every PDAL read occurs on scheduler workers and no provider callback executes on the render thread.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase4 -R copc_provider --output-on-failure
git add src/copc/CopcDatasetProvider.* tests/copc_provider_tests.cpp
git commit -m "feat: preview COPC through the dataset provider"
```

### Task 4.4: Convert COPC to PCI without duplicate tile reads

- [ ] Implement `CopcCacheConverter` using one source traversal into the Phase 3 bucket builder by default. Permit bounded per-tile queries only when a benchmark demonstrates lower wall time or peak disk use for the source.

- [ ] Test point/attribute coverage, deterministic cache output, cancellation, explicit CRS behavior, and cache reopen after the source disappears.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase4 -R copc_cache_conversion --output-on-failure
git add src/import/CopcCacheConverter.* tests/copc_cache_conversion_tests.cpp
git commit -m "feat: convert COPC sources into PCI caches"
```

### Task 4.5: Integrate progressive preview and benchmark

- [ ] On COPC open, show the coarsest available representation first. Continuously run `LodSelector`, reprioritize requests, and feed completed chunks to residency. Never mix PCI and COPC chunks for one logical node.
- [ ] While explicit cache conversion runs, continue rendering COPC. Switch providers only after cache commit and validation.
- [ ] Add `copc_path_benchmark` reporting time-to-first-frame, time-to-target-detail, bytes read, conversion time, and steady-state frame timing for direct and cached paths. Save JSON output under the build directory.

- [ ] Run:

```bash
cmake --build build-phase4 --target copc_path_benchmark
./build-phase4/copc_path_benchmark tests/generated/fixture.copc.laz
```

Expected: valid JSON with both paths and nonzero measurements; no fixed speed ratio assertion.

- [ ] Commit:

```bash
git add src/app src/runtime src/benchmark CMakeLists.txt
git commit -m "feat: integrate progressive COPC preview"
```

### Task 4.6: Phase 4 gate

- [ ] Run all tests with the in-process range server and network access disabled externally:

```bash
cmake -S . -B build-phase4-clean -DBUILD_TESTING=ON
cmake --build build-phase4-clean
ctest --test-dir build-phase4-clean --output-on-failure
```

- [ ] Manually pan rapidly across a large COPC while monitoring request cancellation, memory limits, first-frame latency, and stale chunk suppression. Convert it, switch to PCI, and confirm the rendered bounds and point attributes remain consistent.

**Phase 4 exit criterion:** direct COPC is a complete provider implementation, not a render-loop PDAL call. Cache conversion is optional and selected by measured user value.

---

# Phase 5: GPU Page Residency, Visibility, and Optional Attributes

## Deliverable

The renderer owns a fixed-size GPU point heap, uploads/evicts pages under a per-frame byte budget, selects visible LOD without blocking, and uploads optional attributes only when selected. QRhi remains the portable submission path.

Qt 6.11 QRhi exposes compute pipelines and storage buffers but does not expose a portable indirect-draw command. Therefore this phase first delivers a fully tested QRhi path with GPU-assisted visibility and CPU draw submission. A final measured decision determines whether backend-native Metal indirect command buffers justify a separate renderer interface.

## Phase 5 file map

- Create `src/renderer/GpuPageAllocator.{h,cpp}`: fixed page allocation and generations.
- Create `src/renderer/GpuResidencyManager.{h,cpp}`: upload queue, eviction, frame budget.
- Create `src/renderer/VisibilityResult.{h,cpp}`: backend-neutral visible draw list.
- Create `src/renderer/AttributeResidencyManager.{h,cpp}`: independently resident sidecars.
- Create `shaders/visibility.comp`: optional GPU visibility/LOD pass.
- Create `src/benchmark/SubmissionPathBenchmark.cpp`: QRhi CPU submission versus experimental native path.
- Create `tests/gpu_page_allocator_tests.cpp`, `tests/gpu_residency_tests.cpp`, `tests/attribute_residency_tests.cpp`, and extended renderer smoke tests.

### Task 5.1: Allocate generation-safe GPU pages

- [ ] Define:

```cpp
struct GpuPageHandle {
    std::uint32_t index;
    std::uint32_t generation;
};

class GpuPageAllocator {
public:
    GpuPageAllocator(std::uint32_t pageCount, std::uint32_t pageSizeBytes);
    [[nodiscard]] std::optional<GpuPageHandle> allocate();
    bool release(GpuPageHandle handle);
    [[nodiscard]] bool valid(GpuPageHandle handle) const noexcept;
    [[nodiscard]] std::uint64_t byteOffset(GpuPageHandle handle) const;
};
```

- [ ] Test exhaustion, reuse with incremented generation, stale release rejection, offset overflow, page alignment, and randomized allocate/release sequences against a reference model.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase5 -R gpu_page_allocator --output-on-failure
git add src/renderer/GpuPageAllocator.* tests/gpu_page_allocator_tests.cpp CMakeLists.txt
git commit -m "feat: allocate generation-safe GPU pages"
```

### Task 5.2: Enforce upload and residency budgets

- [ ] Define:

```cpp
struct UploadBudget {
    std::uint64_t maximumBytesPerFrame = 32ULL << 20;
    std::uint64_t maximumResidentBytes = 1ULL << 30;
};

struct PendingUpload {
    ChunkId chunkId;
    GpuPageHandle page;
    std::uint32_t sourceByteOffset;
    std::uint32_t byteCount;
};

class GpuResidencyManager {
public:
    void request(ChunkId id, std::shared_ptr<const PointChunk> chunk,
                 double priority);
    [[nodiscard]] std::vector<PendingUpload>
    beginFrame(std::uint64_t completedFrame);
    void markSubmitted(const PendingUpload &upload,
                       std::uint64_t submissionFrame);
    void markFrameComplete(std::uint64_t completedFrame);
};
```

- [ ] Test priority, exact per-frame byte cap, resident cap, page reuse only after frame completion, duplicate request coalescing, cancellation, pinned visible pages, and recovery after upload failure.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase5 -R gpu_residency --output-on-failure
git add src/renderer/GpuResidencyManager.* tests/gpu_residency_tests.cpp
git commit -m "feat: manage budgeted GPU point residency"
```

### Task 5.3: Move to a fixed QRhi point heap

- [ ] Replace one-buffer-per-chunk resources with one fixed-size QRhi vertex heap divided into allocator pages. Batch uploads through `QRhiResourceUpdateBatch`; split chunks over consecutive pages while keeping draw ranges explicit.
- [ ] Add a fake backend test around the residency manager and a Metal smoke test that cycles more chunks than fit, waits for frame completion, and verifies no stale page generation is drawn.
- [ ] Expose resident bytes, queued upload bytes, pages used, and page faults through `RenderMetrics`.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase5 -R 'gpu_residency|renderer_smoke' --output-on-failure
git add src/renderer src/renderer/metal tests
git commit -m "feat: render from a fixed GPU point heap"
```

### Task 5.4: Add GPU-assisted visibility with a deterministic CPU reference

- [ ] Keep `LodSelector` as the correctness reference. Implement `visibility.comp` to classify hierarchy nodes into a storage-buffer result where supported.
- [ ] In the QRhi path, read back the compact result asynchronously for submission on a later frame. Never wait for readback in `render()`. Fall back to the CPU selector if compute/storage/readback capability is absent.
- [ ] Compare CPU and GPU visible node sets for fixed cameras, frustum boundaries, SSE thresholds, and randomized small hierarchies. Permit ordering differences only after canonical sorting.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase5 -R visibility --output-on-failure
git add shaders/visibility.comp src/renderer/VisibilityResult.* src/renderer/metal tests/visibility_tests.cpp CMakeLists.txt
git commit -m "feat: add GPU-assisted point-cloud visibility"
```

### Task 5.5: Stream optional attributes independently

- [ ] Version the PCI manifest with attribute descriptors while retaining format-version-1 position/color compatibility:

```cpp
enum class AttributeSemantic : std::uint8_t {
    Intensity = 1,
    Classification = 2,
    Custom = 255,
};

enum class AttributeComponentType : std::uint8_t {
    UInt8 = 1,
    UInt16 = 2,
    UInt32 = 3,
    Int8 = 4,
    Int16 = 5,
    Int32 = 6,
    Float32 = 7,
    Float64 = 8,
};

struct AttributeDescriptor {
    std::string name;
    AttributeSemantic semantic;
    AttributeComponentType componentType;
    std::uint8_t componentCount;
    bool normalized;
};
```

Serialize the numeric enum values explicitly and reject unknown required semantics while preserving unknown optional descriptors as metadata.

- [ ] Store intensity and classification sidecars per chunk. `AttributeResidencyManager` requests only the active visualization attribute, uses its own byte budget, and falls back to base RGBA until ready.
- [ ] Test schema round trip, unknown attributes, mismatched point counts, corrupted sidecar checksum, independent eviction, switching active attributes, and rendering while a sidecar is absent.

- [ ] Run and commit:

```bash
ctest --test-dir build-phase5 -R attribute_residency --output-on-failure
git add src/format src/renderer/AttributeResidencyManager.* tests/attribute_residency_tests.cpp shaders
git commit -m "feat: stream optional point attributes"
```

### Task 5.6: Decide, do not assume, native indirect submission

- [ ] Implement `submission_path_benchmark` for the portable QRhi CPU draw loop. Measure CPU submission time for 10, 100, 1,000, and 10,000 visible chunks.
- [ ] Build an experimental Metal-only prototype behind `PCI_EXPERIMENTAL_METAL_INDIRECT=OFF`. It may use QRhi native handles only inside `src/renderer/metal/experimental/`; no native type may cross `RenderViewport`.
- [ ] Record correctness, CPU time, GPU time, frame synchronization complexity, and Qt-version coupling in `docs/renderer_submission_decision.md`.
- [ ] Adopt native indirect submission only if it passes identical-image/visible-count tests and materially reduces frame time at a representative chunk count. Otherwise delete the experiment and retain the benchmark plus decision document.

- [ ] Commit the decision:

```bash
git add src/benchmark docs/renderer_submission_decision.md CMakeLists.txt
git commit -m "docs: decide point-cloud draw submission path"
```

### Task 5.7: Phase 5 gate

- [ ] Run a clean full test:

```bash
cmake -S . -B build-phase5-clean -DBUILD_TESTING=ON
cmake --build build-phase5-clean
ctest --test-dir build-phase5-clean --output-on-failure
```

- [ ] Run a 30-minute camera-path soak over a dataset larger than GPU memory. Log once per second and assert:

  - resident GPU bytes never exceed budget;
  - uploads never exceed the per-frame budget;
  - no stale page generation is submitted;
  - no synchronous file read or GPU readback occurs on the render thread;
  - cancellation and dataset replacement return all pages;
  - frame rendering continues while chunks or attributes are missing.

- [ ] Repeat the renderer smoke and core/provider tests with compute visibility forced off to validate the fallback.

**Phase 5 exit criterion:** datasets larger than GPU memory remain interactive under explicit CPU/GPU budgets, and optional attributes do not inflate the base resident set. Any native indirect path has evidence and an isolated backend boundary.

---

# Cross-phase release checklist

Run this checklist before beginning the next phase:

- [ ] Configure and build from a new build directory.
- [ ] Run all CTest tests, not only tests introduced by the phase.
- [ ] Run AddressSanitizer/UndefinedBehaviorSanitizer for pure C++ libraries.
- [ ] Run ThreadSanitizer for scheduler/controller tests where the Qt/PDAL build supports it.
- [ ] Run the Metal smoke test.
- [ ] Verify cancellation and application shutdown during active work.
- [ ] Verify malformed/truncated input returns an error without partial final output.
- [ ] Record peak importer CPU bytes, runtime CPU cache bytes, GPU resident bytes, and time-to-first-frame.
- [ ] Update user-facing status text and command-line help.
- [ ] Commit only after the phase gate passes.

# Recommended execution order

Implement one phase per branch or worktree:

```text
phase/1-local-pdal-loading
phase/2-in-memory-chunks
phase/3-pci-cache-lod
phase/4-direct-copc
phase/5-gpu-residency
```

Merge each phase before branching the next. Do not begin persistent-format work while Phase 2 chunk transforms or renderer contracts are still changing.
