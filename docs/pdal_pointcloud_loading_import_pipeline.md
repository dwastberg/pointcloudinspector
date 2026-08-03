# PDAL-Based Point Cloud Loading and Import Pipeline

**Target audience:** experienced C++ developers implementing point-cloud import/loading for a high-performance Qt/C++ point-cloud viewer.
**Scope:** PDAL usage, COPC and LAS/LAZ handling, conversion into a custom internal renderer format, and decision guidance for direct source loading versus retiling.
**Recommendation summary:** use **PDAL for source compatibility, metadata inspection, reprojection, filtering, and import/conversion**, but use a **custom internal runtime format and custom runtime loader** for maximum-FPS rendering. PDAL is a C++ library for translating and manipulating point-cloud data, positioned similarly to GDAL for raster/vector data, and it has broad reader/filter/writer coverage. citeturn4search76turn4search78

---

## 1. Executive Recommendation

PDAL should be treated as the **import and compatibility layer**, not as the renderer's hot-path streaming backend. Use PDAL to inspect and read source data, normalize dimensions, optionally reproject/filter/classify/crop, and then write a **viewer-native internal point-cloud format** optimized around GPU upload, chunk residency, LOD traversal, quantization, and indirect rendering. PDAL supports many formats, including LAS/LAZ, COPC, EPT, E57, PLY, PCD, PTS, PTX, I3S, Draco, Arrow/GeoParquet, and more, which makes it the best default compatibility layer for a desktop viewer. citeturn4search77turn4search72

Recommended architecture:

```text
Source files
  ├── LAS / LAZ
  ├── COPC
  ├── EPT
  ├── E57
  ├── PLY / PCD / PTX / PTS
  └── others supported by PDAL
        ↓
PDAL import pipeline
  ├── metadata inspection
  ├── dimension discovery
  ├── reprojection/filtering if requested
  ├── source-specific reading strategy
  └── normalized point stream
        ↓
Internal converter
  ├── spatial tiling / bucketing
  ├── per-chunk quantization
  ├── attribute packing
  ├── hierarchy construction
  ├── LOD generation
  └── chunk compression
        ↓
Viewer-native runtime format
  ├── memory-map / async read friendly
  ├── GPU page-cache friendly
  ├── indirect-draw friendly
  └── suitable for max-FPS rendering
```

The most important separation is:

```text
PDAL = source IO, processing, compatibility, conversion.
Internal loader = runtime streaming, cache residency, GPU upload.
```

This split avoids coupling the renderer to PDAL's pipeline abstractions while still gaining PDAL's mature format support and processing ecosystem. PDAL's C++ API includes stages, readers, writers, filters, `PointTable`, `PointView`, `Dimension`, `Options`, and `StageFactory`, and `StageFactory` can create stages by driver name and infer readers/writers from filenames. citeturn5search89turn5search88turn5search90

---

## 2. Why PDAL Is a Good Import Layer

PDAL is a strong choice because it provides broad source-format coverage and a mature processing pipeline. It normalizes common point dimensions such as X, Y, Z, and Intensity, and exposes readers for many file/database/network formats. This is valuable for a viewer that must read real-world survey/geospatial/scanning data without writing bespoke parsers for every format. citeturn4search77turn4search72

PDAL also provides filters that operate inline on point streams. Filters can remove, modify, reorganize, or add points and dimensions; examples include reprojection, outlier detection, ground classification, height-above-ground computation, clustering, colorization, and more. This is useful for an importer that needs to support preprocessing workflows before data is converted into the renderer's internal format. citeturn5search81turn5search83

For coordinate systems, `filters.reprojection` converts X/Y/Z to a target spatial reference and replaces the original coordinates. PDAL explicitly notes that reprojection can change the precision required to represent coordinates, which is directly relevant when choosing chunk-local quantization and output coordinate scaling. citeturn5search82

---

## 3. Why PDAL Should Not Be the Hot Runtime Loader

A high-performance renderer has very different needs from a general processing pipeline. Runtime loading needs strict frame-budget behavior and direct coordination with GPU residency:

```text
Which chunks are visible this frame?
Which LOD level satisfies the current screen-space error target?
Which chunks are already resident in GPU memory?
Which missing chunks should be requested first?
How many bytes can be uploaded this frame?
Can decoded points go directly into staging buffers or renderer-owned page heaps?
Can attributes be streamed independently?
```

PDAL is designed around readers, filters, writers, point views, and processing pipelines. That is excellent for import and conversion, but it is not the best abstraction for sub-frame-latency GPU page residency and indirect rendering. Instead, the renderer should load its own chunked output format with a custom asynchronous loader.

The internal runtime format should be designed for:

```text
fixed or bounded chunk sizes
per-chunk quantization
GPU-friendly point packing
memory-mapped or async reads
large contiguous GPU point heaps
page table updates
indirect draw command generation
attribute sidecar streaming
LOD hierarchy traversal
cache eviction
```

This is especially important for COPC. COPC is a strong source/runtime-preview format because it is a LAZ 1.4 file organized as a clustered octree and supports spatial subset access, but COPC's storage layout is not necessarily the optimal layout for your renderer's GPU page cache. citeturn4search74turn4search84

---

## 4. Internal Format Target

The importer should produce a custom dataset package. A single-file container is preferred for filesystem performance, but a directory format is easier to implement/debug initially.

Directory-based prototype:

```text
dataset.pci/
  metadata.json
  hierarchy.pcih
  chunks.pcichunk
  attributes.pcia
  lod.pcih
  statistics.json
  source_manifest.json
```

Single-file production container:

```text
dataset.pci
  FileHeader
  MetadataBlock
  SourceManifestBlock
  AttributeSchemaBlock
  HierarchyBlock
  ChunkDirectoryBlock
  ChunkPayloadBlocks
  OptionalAttributeSidecarBlocks
  Checksums / footer
```

Each internal chunk should be independently loadable and directly transcodable/uploadable to the GPU:

```cpp
struct PciChunkHeader
{
    uint64_t chunkId;
    uint32_t level;
    uint32_t pointCount;

    double worldOriginX;
    double worldOriginY;
    double worldOriginZ;

    float localMin[3];
    float localMax[3];

    float quantScale[3];
    float quantOffset[3];

    uint32_t positionFormat;
    uint32_t colorFormat;
    uint32_t attributeMask;

    uint64_t payloadOffset;
    uint64_t payloadSizeCompressed;
    uint64_t payloadSizeUncompressed;
};
```

Keep authoritative coordinates and CRS metadata in double precision, but render from chunk-local quantized coordinates. LAS/COPC coordinates are based on scaled integer models, and PDAL's COPC documentation emphasizes that LAS stores X/Y/Z as scaled integers and that preserving scale/offset can be important for precision. citeturn4search81turn5search93

Recommended GPU payload format:

```cpp
struct GpuPoint16
{
    uint16_t x;
    uint16_t y;
    uint16_t z;
    uint16_t intensityClassFlags;
    uint32_t rgba;
    uint16_t normalOctOrAttr0;
    uint16_t attr1;
};
```

A 16-byte point format is a good starting point for alignment, bandwidth, and shader simplicity.

---

## 5. Source Detection and Dispatch

Use explicit source detection, with PDAL inference as a fallback. PDAL's `StageFactory` provides `inferReaderDriver()` and creates stages by driver name, with stages owned by the factory. citeturn5search88turn5search90

```cpp
enum class SourceKind
{
    Copc,
    Las,
    Laz,
    Ept,
    E57,
    Ply,
    Unknown
};

ImportResult PdalImportManager::import(const ImportJob& job)
{
    SourceKind kind = detectSourceKind(job.sourcePath);

    switch (kind)
    {
        case SourceKind::Copc:
            return importCopc(job);
        case SourceKind::Las:
        case SourceKind::Laz:
            return importLas(job);
        default:
            return importGeneric(job);
    }
}
```

Recommended driver mapping:

```text
.copc.laz  -> readers.copc
.las       -> readers.las
.laz       -> readers.las
EPT        -> readers.ept
.e57       -> readers.e57
.ply       -> readers.ply
.pcd       -> readers.pcd
.pts/.ptx  -> readers.pts / readers.ptx
```

PDAL has explicit readers for COPC, EPT, E57, LAS, PLY, PCD, PTS, PTX, and many more, so most source formats can initially flow through a generic PDAL import path. citeturn4search77turn4search72

---

## 6. Metadata Inspection Pass

Before importing all points, inspect the source and build a `SourceMetadata` object:

```cpp
struct SourceMetadata
{
    uint64_t pointCount;
    Bounds3d bounds;
    std::string spatialReferenceWkt;
    std::string sourceDriver;

    bool hasRgb;
    bool hasIntensity;
    bool hasClassification;
    bool hasGpsTime;
    bool hasNormals;

    std::vector<SourceDimension> dimensions;
    SourceLasInfo lasInfo;
    SourceCopcInfo copcInfo;
};
```

Metadata should include:

```text
point count
bounds
source CRS
LAS version / point format where applicable
available dimensions
scale and offset
VLR/EVLR metadata if relevant
RGB availability
intensity availability
classification availability
GPS time availability
extra dimensions
estimated density / spacing
```

For LAS/COPC import, preserve original LAS scale/offset, point format, version, and relevant VLR metadata in `source_manifest.json`. PDAL's LAS writer documentation warns that LAS scale/offset are not automatically preserved unless using forwarding options, which is a useful reminder that source scale/offset should be captured explicitly even if not reused as the internal quantization. citeturn5search93turn5search95

---

## 7. Coordinate System and Reprojection Policy

The importer should support three coordinate policies:

```text
PreserveSourceCrs:
  Store source CRS and coordinates as-is, then render camera-relative.

ReprojectToProjectCrs:
  Use PDAL filters.reprojection to convert to the project CRS during import.

NormalizeToLocalEngineeringFrame:
  Store source CRS and transformation metadata, but use local coordinates internally.
```

If reprojection is requested, insert `filters.reprojection` after the reader stage. PDAL's reprojection filter converts X/Y/Z dimensions to a new spatial reference, and if original coordinates must be preserved, `filters.ferry` can be used before reprojection to copy them into other dimensions. citeturn5search82

Example reprojection pipeline block:

```json
[
  {
    "type": "readers.las",
    "filename": "input.laz"
  },
  {
    "type": "filters.reprojection",
    "in_srs": "EPSG:3006",
    "out_srs": "EPSG:4978"
  }
]
```

After reprojection, compute internal chunk bounds and quantization from the **post-reprojection coordinates**, not from source LAS scale/offset. PDAL notes that reprojection can significantly change precision requirements, so quantization must be recomputed after coordinate transformation. citeturn5search82

---

## 8. COPC Import Path

### 8.1 COPC Characteristics

COPC is a LAZ 1.4 file organized as a clustered octree, with hierarchy information that lets readers select and seek through the file rather than sequentially reading everything. COPC is designed for spatial subset access and cloud/range-request workflows. citeturn4search74turn4search84

PDAL's `readers.copc` supports COPC files, incremental loading, spatial filtering, streaming operations, `bounds` queries, and remote file specifications such as HTTP and cloud object storage sources. citeturn4search81turn4search82

### 8.2 Recommendation: Use COPC Tiling as Source Acceleration, Retile for Runtime Cache

Do not blindly adopt COPC's internal tiling as the renderer-native tiling. Use COPC hierarchy as a **source acceleration structure**, then retile into your internal format for maximum performance.

```text
Direct COPC path:
  fast open, preview, cloud streaming, one-off inspection

Retiled internal path:
  repeated use, maximum FPS, optimized GPU upload, stable chunk sizes
```

COPC's internal layout is optimized for cloud-optimized LAZ storage and spatial access. Your renderer's internal layout should be optimized for GPU page size, draw grouping, quantization, attribute streaming, LOD quality, and cache eviction. citeturn4search74turn4search84

### 8.3 COPC Retile Pipeline

Recommended COPC import flow:

```text
COPC file
  └── inspect source metadata and hierarchy
        └── choose internal root bounds and LOD policy
              └── generate internal tile bounds
                    └── query COPC by tile bounds through PDAL
                          └── normalize attributes
                                └── quantize into internal chunk
                                      └── write leaf chunk
                                            └── build parent LOD chunks
```

For each internal tile, execute a PDAL COPC reader with bounds:

```json
[
  {
    "type": "readers.copc",
    "filename": "input.copc.laz",
    "bounds": "([319000,320000],[6390000,6391000],[0,200])"
  }
]
```

With optional reprojection:

```json
[
  {
    "type": "readers.copc",
    "filename": "input.copc.laz",
    "bounds": "([319000,320000],[6390000,6391000],[0,200])"
  },
  {
    "type": "filters.reprojection",
    "out_srs": "EPSG:3006"
  }
]
```

PDAL's COPC `bounds` option selects 2D/3D extents and is a natural mechanism for tile-by-tile import. citeturn4search81turn4search82

### 8.4 COPC Direct Preview Mode

For excellent UX, support a direct COPC preview mode:

```text
State 1: Source-only
  - no internal cache exists
  - use COPC hierarchy and PDAL/custom COPC reads
  - show progressively refined preview

State 2: Cache-building
  - background retile is running
  - renderer may mix temporary source chunks and internal cached chunks

State 3: Cached
  - renderer uses internal .pci chunks only
  - COPC is retained as source reference
```

For a first implementation, PDAL `readers.copc` is adequate for bounded reads and conversion. For a later high-performance direct preview path, consider a custom COPC reader or `copc-lib`, which provides C++ reader/writer interfaces for Cloud Optimized Point Clouds and uses `laz-perf`. citeturn4search70turn4search81

---

## 9. LAS/LAZ Import Path

### 9.1 LAS/LAZ Characteristics

Plain LAS/LAZ files should be treated primarily as sequential interchange files, unless an external spatial index exists. PDAL's LAS reader supports ASPRS LAS 1.0–1.4, and LASzip/LAZperf support is enabled through the reader when available. citeturn4search77turn4search72

LAS is the standard interchange format for LiDAR, but it is not an ideal renderer runtime structure. PDAL's LAS tutorial covers LAS versions, point formats, coordinate systems, extra dimensions, scaling, compression, and metadata, and notes that PDAL's LAS support is important but not every LAS feature is universally supported, with waveform point formats specifically called out as unsupported. citeturn5search94turn5search96

### 9.2 LAS Retile Pipeline

Recommended LAS/LAZ import flow:

```text
LAS/LAZ file
  └── PDAL readers.las
        ├── metadata pass
        ├── optional sample/statistics pass
        ├── sequential streaming pass
        │     ├── compute internal tile id
        │     ├── append to tile bucket
        │     └── spill buckets to temp files if needed
        ├── finalize leaf chunks
        └── build parent LOD chunks
```

Basic LAS pipeline:

```json
[
  {
    "type": "readers.las",
    "filename": "input.laz"
  }
]
```

LAS with reprojection:

```json
[
  {
    "type": "readers.las",
    "filename": "input.laz"
  },
  {
    "type": "filters.reprojection",
    "out_srs": "EPSG:3006"
  }
]
```

LAS with filtering:

```json
[
  {
    "type": "readers.las",
    "filename": "input.laz"
  },
  {
    "type": "filters.expression",
    "expression": "Classification != 7"
  }
]
```

PDAL filters can remove, modify, reorganize, classify, and add dimensions, making this a natural extension point for import-time filtering options. citeturn5search81turn5search83

### 9.3 Disk-Backed Bucketing for Large LAS Files

For large LAS/LAZ files, assume the importer cannot hold every point in memory. Use disk-backed bucket files:

```text
Pass 1:
  inspect metadata and choose root/tile grid

Pass 2:
  stream points sequentially
  compute tile id
  append compact temporary record to bucket
  spill full buckets to disk

Pass 3:
  for each bucket:
      sort by Morton/Hilbert key
      split into target-size chunks
      quantize per chunk
      write final chunk payload

Pass 4:
  build upper LOD levels
```

PDAL can produce multiple `PointView`s through filters such as splitter/chipper/divider in some writer workflows, but renderer chunking is specific enough that implementing your own spatial bucket builder is preferable. PDAL's LAS writer documentation notes that multiple `PointView`s are often produced by `filters.splitter`, `filters.chipper`, or `filters.divider`, but the renderer's chunk-size and GPU-page constraints should drive final tiling. citeturn5search93turn5search95

---

## 10. Connecting PDAL to the Internal Writer

There are two viable implementation patterns.

### Option A: Custom PDAL Writer Stage

Implement `writers.pci`, a custom PDAL writer stage:

```text
readers.las / readers.copc / readers.e57 / ...
  -> optional filters
  -> writers.pci
```

Advantages:

```text
clean PDAL-native pipeline composition
supports all PDAL readers and filters uniformly
works well with command-line conversion/debugging
centralizes dimension mapping and internal writing logic
```

PDAL documentation notes that if you implement your own reader/writer conforming to PDAL stages, you can build read-filter-write pipelines using PDAL's pipeline infrastructure instead of coding everything manually. citeturn5search91

Suggested custom writer responsibilities:

```text
writers.pci
  ├── receive PDAL point stream
  ├── map dimensions into internal attribute schema
  ├── assign points to spatial buckets
  ├── spill buckets to temporary files
  ├── finalize quantized chunks
  ├── build hierarchy and LOD
  └── write metadata/source manifest
```

### Option B: C++ Importer Consuming PDAL PointViews

Use PDAL's C++ API to create readers/filters and consume `PointView` output directly. PDAL examples show using `StageFactory`, `Options`, `PointTable`, and `PointView` to create stages and execute pipelines. citeturn5search87turn5search89

Sketch:

```cpp
pdal::StageFactory factory;

pdal::Stage* reader = factory.createStage("readers.las");

pdal::Options readerOptions;
readerOptions.add("filename", inputPath.string());
reader->setOptions(readerOptions);

pdal::PointTable table;
reader->prepare(table);

pdal::PointViewSet views = reader->execute(table);

for (const pdal::PointViewPtr& view : views)
{
    for (pdal::PointId i = 0; i < view->size(); ++i)
    {
        double x = view->getFieldAs<double>(pdal::Dimension::Id::X, i);
        double y = view->getFieldAs<double>(pdal::Dimension::Id::Y, i);
        double z = view->getFieldAs<double>(pdal::Dimension::Id::Z, i);

        // Map attributes, bucket, quantize later.
    }
}
```

This is easier initially, but be careful with very large datasets because `execute()` can materialize large `PointView`s depending on the pipeline. For huge imports, prefer streamable stages or a custom PDAL writer that can spill buckets incrementally.

---

## 11. Attribute Mapping

Define a strict internal attribute schema. Known attributes get compact fixed streams; unknown attributes are preserved in optional sidecars.

```cpp
enum class PciAttributeSemantic
{
    Position,
    Color,
    Intensity,
    Classification,
    ReturnNumber,
    NumberOfReturns,
    GpsTime,
    ScanAngle,
    UserData,
    PointSourceId,
    Normal,
    ExtraScalar,
    ExtraVector
};
```

Typical mapping:

```text
PDAL X/Y/Z              -> Position
PDAL Red/Green/Blue     -> Color
PDAL Intensity          -> Intensity
PDAL Classification     -> Classification
PDAL GpsTime            -> GPS time
PDAL ReturnNumber       -> Return number
PDAL NumberOfReturns    -> Number of returns
PDAL ScanAngleRank      -> Scan angle / scan angle rank
```

PDAL normalizes many common dimensions, but extra dimensions need special treatment. For LAS 1.4, extra-byte metadata is typically available through VLR metadata; for LAS 1.0–1.3, PDAL may not know the meaning of arbitrary extra bytes unless they are explicitly described. citeturn4search77turn5search98

Recommended extra-dimension policy:

```text
Known renderer attributes:
  store compactly in fixed streams

Unknown scalar/vector attributes:
  store in sidecar blocks
  preserve name, type, scale, offset if known
  expose in UI as optional visualization dimensions
  do not upload unless selected
```

---

## 12. Quantization Strategy

Use **per-chunk quantization**, not global quantization.

For each chunk:

```cpp
chunkOrigin = chunkBounds.min;

chunkScale.x = (chunkBounds.max.x - chunkBounds.min.x) / 65535.0;
chunkScale.y = (chunkBounds.max.y - chunkBounds.min.y) / 65535.0;
chunkScale.z = (chunkBounds.max.z - chunkBounds.min.z) / 65535.0;

uint16_t qx = round((x - chunkOrigin.x) / chunkScale.x);
uint16_t qy = round((y - chunkOrigin.y) / chunkScale.y);
uint16_t qz = round((z - chunkOrigin.z) / chunkScale.z);
```

Store:

```text
chunk origin: double3
chunk scale: float3 or double3
quantized point position: uint16x3
```

Per-chunk quantization improves precision and reduces GPU bandwidth. Keep source LAS/COPC scale/offset in metadata, but do not use source scale/offset as the runtime GPU representation unless it happens to align with renderer chunking. PDAL's documentation around LAS scale/offset and COPC scaled coordinates reinforces that precision metadata must be handled explicitly. citeturn4search81turn5search93

---

## 13. Hierarchy and LOD Construction

Construct leaves first, then parent LODs.

Leaf construction:

```text
COPC:
  query by internal tile bounds
  decode source points through PDAL/custom reader
  write internal leaf chunks

LAS/LAZ:
  sequentially stream all points
  bucket into internal tiles
  spill to temp files if needed
  finalize internal leaf chunks
```

LOD construction strategies:

```text
Random sampling:
  fastest, lower quality

Voxel representative:
  good default; stable density and predictable spacing

Color-filtered voxel representative:
  better visual quality for low LODs

Poisson-like selection:
  good quality, more expensive
```

Recommended initial LOD builder:

```text
for each parent node:
    create voxel grid over parent bounds
    accumulate child points into occupied voxels
    emit one representative per occupied voxel
    average RGB/intensity where appropriate
    choose classification by majority or configurable policy
```

This aligns with a hybrid voxel-point renderer, where inner nodes contain representative voxels/points and leaves contain full-resolution or near-full-resolution points.

---

## 14. COPC Internal Tiling: Use or Retile?

Use COPC's internal tiling as a **source acceleration structure**, but retile into your own internal format for peak renderer performance.

```text
Use COPC tiling directly when:
  - first opening a file
  - previewing remote/cloud data
  - user may not revisit the dataset
  - no internal cache exists
  - rapid initial visualization matters more than peak FPS

Retile when:
  - the dataset is local or repeatedly used
  - maximum FPS matters
  - stable GPU page sizes matter
  - custom LOD quality matters
  - attribute sidecar streaming matters
  - predictable draw grouping matters
```

COPC is designed for spatial access and cloud-optimized LAZ storage; your internal format should be designed for renderer execution. COPC's hierarchy and storage are valuable for seeking source data, while your internal hierarchy should be tailored for GPU upload, cache residency, LOD traversal, and indirect draw. citeturn4search74turn4search84

Recommended three-state behavior:

```text
State 1: Source-only
  - render direct COPC preview
  - no cache exists

State 2: Cache-building
  - retile in background
  - renderer can mix source and cached chunks

State 3: Cached
  - renderer uses internal format only
  - COPC retained as source reference
```

---

## 15. Suggested Class Design

```cpp
class ImportJob
{
public:
    std::filesystem::path sourcePath;
    std::filesystem::path outputPath;
    ImportOptions options;
};

class PdalImportManager
{
public:
    ImportResult import(const ImportJob& job);

private:
    ImportResult importCopc(const ImportJob& job);
    ImportResult importLas(const ImportJob& job);
    ImportResult importGeneric(const ImportJob& job);
};

class SourceInspector
{
public:
    SourceMetadata inspectWithPdal(const std::filesystem::path& path);
};

class AttributeMapper
{
public:
    PciAttributeSchema buildSchema(const SourceMetadata& source);
    NormalizedPoint mapPoint(const pdal::PointView& view, pdal::PointId id);
};

class SpatialBucketBuilder
{
public:
    void begin(const Bounds3d& rootBounds, const BucketOptions& options);
    void appendPoint(const NormalizedPoint& point);
    void flush();
    std::vector<BucketRef> finalizeBuckets();
};

class PciDatasetWriter
{
public:
    void beginDataset(const SourceMetadata&, const ImportOptions&);
    void writeLeafChunk(const ChunkBuildResult&);
    void writeLodChunk(const ChunkBuildResult&);
    void finalize();
};

class LodBuilder
{
public:
    std::vector<ChunkBuildResult> buildFromLeaves(const std::vector<ChunkRef>& leaves);
};
```

---

## 16. Recommended Implementation Phases

### Phase 1: Generic PDAL-to-internal conversion

- Implement `SourceInspector`.
- Implement `readers.las` and `readers.copc` import stubs.
- Map XYZ/RGB/intensity/classification.
- Write uncompressed internal chunks.
- Build a simple octree hierarchy.

### Phase 2: LAS/LAZ production import

- Add sequential disk-backed bucketing.
- Add per-chunk quantization.
- Add chunk compression.
- Add Morton/Hilbert sorting inside chunks.
- Add parent LOD generation.

### Phase 3: COPC-optimized import

- Use COPC bounds queries for internal tile extraction.
- Add source-COPC hierarchy metadata capture.
- Add background retile mode.
- Preserve source COPC path/hash for cache invalidation.

### Phase 4: Direct COPC preview

- Render from COPC before retile completes.
- Initially use PDAL bounded COPC reads.
- Later evaluate `copc-lib` or custom COPC/laz-perf path for faster direct preview. citeturn4search70turn4search81

### Phase 5: Advanced attributes and processing

- Add extra dimension sidecars.
- Add reprojection UI.
- Add filtering pipeline presets.
- Add density/statistics generation.
- Consider `filters.hexbin` for actual footprint/density estimation, as it can produce a boundary and density metadata rather than only rectangular file bounds. citeturn5search84

---

## 17. Practical Caveats

### Scale and offset

Do not assume LAS scale/offset are preserved automatically. Store source scale/offset explicitly and compute your own internal per-chunk quantization. PDAL's LAS writer documentation explicitly warns that scale/offset are not preserved unless using forwarding options such as `forward`, `scale`, and `offset`. citeturn5search93turn5search95

### Extra dimensions

Do not assume all extra bytes are self-describing. LAS 1.4 commonly carries extra-byte descriptions in VLRs; older LAS versions may not. PDAL discussions around extra dimensions show that older LAS extra bytes may require explicit naming/types because PDAL cannot infer opaque dimensions reliably. citeturn5search98

### Waveforms

If waveform LAS support matters, check PDAL limitations carefully. PDAL's LAS tutorial notes that PDAL does not support point formats that store waveform data. citeturn5search94turn5search96

### Memory behavior

Avoid full materialization for huge files. If using `Stage::execute()` directly, verify memory behavior on multi-hundred-million-point sources. For production imports, a streamable custom PDAL writer stage or incremental bucket-spilling importer is safer.

### Reprojection precision

Recompute chunk bounds and quantization after reprojection. PDAL's reprojection documentation explicitly warns that coordinate reprojection can change the precision necessary for output formats. citeturn5search82

---

## 18. Final Recommended Design

Build the feature around this architecture:

```text
PdalImportManager
  ├── SourceInspector using PDAL
  ├── CopcImportPath using readers.copc + bounds queries
  ├── LasImportPath using readers.las + sequential disk-backed bucketing
  ├── GenericImportPath using inferred PDAL readers
  ├── AttributeMapper
  ├── Quantizer
  ├── SpatialBucketBuilder
  ├── PciDatasetWriter
  └── LodBuilder
```

Use PDAL for:

```text
source reading
metadata extraction
dimension normalization
reprojection
filtering
classification/cropping workflows
format compatibility
```

Use your own code for:

```text
internal format
renderer-native chunking
per-chunk quantization
GPU point packing
LOD generation
chunk compression
runtime stream metadata
GPU page-cache compatibility
```

The final position is:

> **PDAL is the right import backbone, but not the right runtime renderer backend. COPC's internal tiling should be used as a source acceleration structure and direct-preview path, while production/max-FPS rendering should use a retiled internal cache designed specifically for the renderer.**
