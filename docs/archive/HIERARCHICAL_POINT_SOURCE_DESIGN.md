# Hierarchical point-source and residency design

Status: archived 2026-07-31; this 2026-07-18 design record is superseded as active architecture guidance by the implemented residency system and the Point Cloud Inspector Modernization Refactoring Guide.

Status: foundation MVP, document-wide residency coordination, deterministic
stress coverage, and local qualification tooling complete, 2026-07-18;
representative production-file qualification remains pending.

## Decision

Use PDAL's public COPC and EPT readers as spatial/resolution query engines,
behind application-owned logical node IDs and application-owned CPU/GPU
caches.

The viewer does not include PDAL private reader headers and does not build a
second persistent octree. A node is identified by `(level, x, y, z)` within a
layer. Its bounds are an implicit subdivision of the source bounds, and its
geometric error is the resolution used for the PDAL query. The GPU key adds the
layer ID and block index, so decoded object addresses never become identities.

COPC is the preferred preparation format for large single-file datasets. EPT
remains a supported hierarchical input for existing tiled and remote
deployments. Flat LAS/LAZ is not described as out of core; its separately
documented capped path remains available for small and medium inputs.

## Alternatives considered

### PDAL bounds/resolution queries — selected

PDAL `readers.copc` and `readers.ept` both expose bounds and resolution filters,
streaming execution, and configurable request concurrency. This keeps point
dimension handling, LAZ decompression, spatial-reference metadata, and remote
I/O in the existing dependency.

The installed PDAL 2.10.2 public C++ classes do not expose a stable public API
for walking raw COPC/EPT hierarchy pages. Their query methods are public stage
options, while hierarchy internals are private implementation details. The
viewer therefore maps queries to its own stable logical IDs instead of
including private PDAL headers or treating decoded blocks as nodes.

Trade-off: logical grid nodes do not necessarily match physical COPC nodes.
PDAL can consequently revisit hierarchy pages or compressed nodes for adjacent
queries. Cache hit rates and HTTP byte traffic need continued profiling on
production-scale data.

### Focused native COPC/EPT hierarchy adapter — deferred

A native adapter could expose physical node keys, hierarchy page sizes, and
more exact request prioritization. It would also duplicate parsing, HTTP range
handling, decompression coordination, error recovery, and format-version work
already supplied by PDAL. There is not yet measured evidence that the selected
adapter is too slow or too wasteful, so this additional implementation and
dependency surface is not justified.

If production traces show excessive duplicate range traffic, this is the first
alternative to reconsider. Point-field conversion can remain in
`PdalPointMapping.cpp` even if hierarchy traversal moves to a focused adapter.

### Application-owned persistent hierarchy — rejected for hierarchical input

Building a custom permanent octree for COPC/EPT would duplicate the principal
feature of both formats, delay first display, consume extra disk space, and add
format migration and cleanup requirements. It is not used for COPC/EPT.

A versioned temporary hierarchy remains a possible future enhancement for flat
inputs, but it should be a separate design and migration. The present fallback
uses a clear in-memory cap instead.

## Source behavior

`PdalHierarchicalPointSource` supports `readers.copc` and `readers.ept`. Opening
a source reads only PDAL preview metadata and one representative root query.
The application hierarchy is implicit, so opening does not materialize a
record for every leaf.

Each logical node query supplies:

- the node's three-dimensional bounds;
- a resolution derived from source extent, target points per node, and level;
- at most 65,536 representative decoded points, reduced further when a small
  CPU cache budget is configured;
- deterministic reservoir sampling if PDAL returns more points than the node
  cap.

Child bounds are half-open except at the source maximum. A source point belongs
to exactly one child at a level, including points on subdivision planes.

The CLI accepts local paths and PDAL-supported URL strings. The desktop file
dialog remains a local-file chooser; remote sources currently need to be
passed on the command line.

### Flat LAS/LAZ fallback

Flat `readers.las` inputs continue through the progressive streaming importer.
They retain their sampled decoded blocks and are therefore not out of core.
`--max-points` is the explicit supported-size cap and defaults to 10,000,000
points. Raising it deliberately raises retained CPU memory. For a large flat
source, convert it before interactive viewing, for example:

```sh
pdal translate input.laz output.copc.laz writers.copc
```

No acceptance claim for hierarchical residency applies to this flat path.

## CPU residency contract

`PointCloudScene` owns a lightweight source adapter and `DecodedBlockCache`,
not all nodes in the cloud. `PointCloudDocument` owns a shared
`HierarchyResidencyCoordinator`; `--cpu-cache-mb` configures its total decoded
point budget, not a multiplier applied to every layer.

Each retained coarse root is reserved first. The remaining bytes are divided
equally between visible hierarchical layers. A hidden layer is reduced to its
root allowance, and adding, hiding, showing, or removing a layer immediately
rebalances and trims the per-scene caches. If retained roots alone exceed the
configured budget, they form an explicit fixed coverage allowance that cannot
be evicted without losing the coarse representation.

Accounting uses the capacities of the `GpuPoint` and `PointAttributes`
vectors, so reserved allocation is included. Fixed C++ object/container
overhead is small and is not counted as point storage. The root representative
is retained as the coverage fallback and counts toward the cache budget.

Only one application node query runs at a time per scene, and the document
coordinator admits at most two queries globally by default. Admission is FIFO
across layer workers, preventing a busy layer from repeatedly overtaking a
waiting layer. Initial COPC/EPT root imports use the same permits but do not
claim a persistent cache share before their scene enters the document. The
application keeps the admission queue across transactional document
replacement, so background work in an outgoing document cannot multiply the
limit while its replacement is loading.

The per-node point cap is at most one sixteenth of the configured document
cache's nominal point capacity, up to the 65,536-point ceiling. Active decoding
has a separate bounded
allowance consisting of one capped `PointSample` reservoir, the node being
partitioned, PDAL's 4,096-point streaming table, and PDAL reader internals.
The COPC adapter asks PDAL for at most two internal requests. Thus peak decoded
memory is the document cache budget, retained-root/protected-lease allowances,
and at most two bounded active node decodes, rather than one cache and active
decode allowance per hierarchical layer.

`PointCloudNodePayloadPtr` is also the pin mechanism. Current-frame draws and
in-flight picks retain only their referenced payload blocks. GPU uploads copy
into QRhi-owned staging data and do not leave a strong CPU block reference.
Unleased LRU entries are evicted and transparently re-queried on a later visit.

Camera reprioritization replaces the pending queue and requests stop on an
obsolete active query. PDAL callbacks check the stop token per streamed point.
One failed node is not retried in a tight loop; the error is surfaced through
the renderer failure path.

## Residency instrumentation contract

The low-frequency `RenderMetrics` snapshot now exposes:

- decoded-cache hits, misses, insertions, replacements, evictions, current
  bytes, configured document budget, and sampled peak bytes;
- scene decode requests queued, admitted, completed, cancelled, and failed;
- global FIFO requests/admissions/cancellations, current/peak active and queued
  work, wait time, and estimated current/peak decoder bytes;
- PDAL node queries completed/cancelled/failed, source points visited, decoded
  points/bytes produced, estimated decoded bytes requested, and query time;
- current and peak process RSS using `GetProcessMemoryInfo` on Windows,
  Mach/`getrusage` on macOS, and `/proc`/`getrusage` on Linux.

The decoder-byte value is a conservative application-owned estimate covering
the point-sample reservoir and decoded output vectors. PDAL reader internals,
decompression state, HTTP buffers, allocator overhead, and Qt allocations are
instead captured by whole-process RSS. Cache and decoder peaks explain the
known application-controlled portions; peak RSS is the authoritative process
high-water mark.

PDAL's public COPC/EPT stage API does not report physical file or HTTP range
bytes fetched, so `fetchedBytesKnown` remains false for this adapter. The
viewer does not relabel estimated decoded output as network traffic. Actual
remote fetched bytes must still be collected from a controlled HTTP
server/proxy during production qualification.

`pci_residency_bench` exercises the same scene worker and document residency
coordinator against local hierarchical files. It repeatedly revisits a working
set larger than the configured cache and reports cache peaks/evictions, source
reloads, cancellation latency, decoder allowance, and sampled/process
high-water RSS in both human-readable and `RESIDENCY_RESULT` forms. CTest runs
it against a generated 64,000-point local hierarchy with 160 discovered nodes
and a greater-than-10× cache working set. The deterministic CPU suite separately
validates a greater-than-20× decoded working set and rapid cancellation without
depending on a particular file. The native GPU stress test alternates
constrained hierarchical layers for greater-than-10× cumulative residency
churn and verifies that current and peak point-buffer bytes never exceed the
configured budget.

## GPU residency contract

`UploadScheduler` owns a global stable-key GPU LRU with a default 512 MiB point
buffer budget, configured by `--gpu-cache-mb`. It tracks global and per-layer
bytes and points incrementally. Removed layers are released immediately.

Every frame protects currently selected coarse/detailed blocks, then evicts
the least recently visible and least recently used unprotected buffers before
new uploads. Upload staging is limited to 48 MiB per frame. Renderer uniform
buffers, pipeline objects, the swapchain, and picker targets are fixed renderer
allocations outside this point-buffer budget.

Selection is capped below raw GPU capacity to leave one ninth of the point
residency available during an octree parent-to-eight-children transition.
Decoded visible children are prefetched while the drawable parent remains
protected. The selector commits refinement only when every visible child has a
GPU buffer and the children fit the point budget. This preserves coarse
coverage rather than replacing a parent with temporary empty space.

## Selection

`RenderSelection` is platform independent. It applies frustum visibility,
projected geometric error, point budget, and separate refine/coarsen thresholds.
Visible child requests are ordered by projected error. It returns stable node
IDs for requests and drawing; QRhi resources are resolved later.

The initial implementation uses an implicit octree and a point budget. It does
not yet use source-reported physical node counts because those are not exposed
by the chosen public PDAL API. Empty logical nodes are cached as empty payloads,
which prevents repeated queries during the same residency period.

## Validation evidence

The automated suite now covers:

- COPC metadata inspection and root/child bounds-resolution queries through
  PDAL 2.10.2;
- an eight-octant fixture proving boundary ownership and bounded node payloads;
- cancellation before a PDAL query and cancellation/reprioritization of an
  active scene query;
- cache LRU order, frame leases, byte/point counters, and a working set over
  ten times its budget;
- document-wide cache allocation, hidden-layer rebalancing, FIFO admission,
  and a multi-layer test proving the global decode limit;
- cache/source/decode counter accounting and coherent operating-system process
  RSS diagnostics;
- GPU eviction order and protected-block behavior without requiring a GPU;
- coarse fallback, atomic child replacement, point budgets, visibility, and
  refinement hysteresis.

On the local Debug build, the fixture test performing a root query plus eight
child COPC queries completes in roughly 0.2–0.3 seconds. This tiny eight-point
fixture validates behavior, not production performance. PDAL's public stage
API does not expose fetched-byte counters, so byte-range efficiency must be
measured at the HTTP server/proxy for representative deployments.

HTTP COPC and EPT are supported by the selected PDAL stages but are not placed
in the hermetic default test suite: it must not depend on an external server.
Before a production remote-data release, run a controlled local range server
and representative EPT/COPC endpoints, recording time to first root, requested
bytes, cache-revisit traffic, cancellation latency, and peak RSS. Those are
deployment qualification measurements, not a reason to introduce a custom
hierarchy pre-emptively.

## Failure recovery and compatibility

- Minimum supported PDAL is the build's required 2.10 series or newer.
- COPC format handling and remote range behavior are delegated to PDAL/libcurl.
- EPT ingestion is retained through `readers.ept`; no EPT writer is required by
  the application.
- Malformed metadata and node-query failures include the source name and are
  reported to the existing load/render error callbacks.
- Cache eviction is recoverable because the source path and logical node ID are
  sufficient to reproduce a query.
- A future native adapter can implement `PointCloudDataSource` without changing
  scene, selection, or GPU cache identities.

## Relevant specifications and reader documentation

- [PDAL COPC reader](https://pdal.io/en/stable/stages/readers.copc.html)
- [PDAL EPT reader](https://pdal.io/en/stable/stages/readers.ept.html)
- [PDAL COPC writer](https://pdal.io/en/stable/stages/writers.copc.html)
- [COPC format overview and specification](https://copc.io/)
