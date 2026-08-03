# Local Point Page Store v1

Status: archived 2026-07-31; this 2026-07-18 Release F design record is superseded as active architecture guidance by the implemented page store and the Point Cloud Inspector Modernization Refactoring Guide; schema v1 remains supported.

**Status:** approved implementation contract for Release F (2026-07-18).

## Purpose and scope

The local point page store makes ordinary local LAS/LAZ sources reloadable by
spatial page instead of retaining their complete decoded `PointBlock` vectors.
It is an application cache, never a modification of the source file. The v1
adapter implements `PointCloudDataSource`, so the existing hierarchy selection,
decoded LRU, parent fallback, upload throttling, and picking paths can consume
it. Release G subsequently replaced the adapter's temporary per-scene
residency and planning ownership without changing this v1 disk contract.

This format is private cache data, not an interchange format. A reader must
reject anything it cannot validate and rebuild it from the unchanged source.

## Cache identity and location

The GUI supplies Qt's platform-native `QStandardPaths::CacheLocation`, with a
`point-pages` child directory. Tests and embedders may supply another directory.
The source directory is never required to be writable.

The source fingerprint is SHA-256 over:

1. the canonical UTF-8 source path;
2. source byte size and modification timestamp;
3. LAS/LAZ header-derived driver, point count, bounds, dimensions, and CRS;
4. up to 64 KiB sampled at the beginning, middle, and end of the source; and
5. the page-store schema and layout parameters.

The final directory name is the lower-case fingerprint plus `.pcipages`. A
source change therefore selects a different cache entry. The manifest repeats
the fingerprint and source identity so a renamed or manually copied entry is
not trusted solely because of its filename.

## Directory and commit protocol

Each committed entry contains exactly:

```text
<fingerprint>.pcipages/
    manifest.pci
    payload.bin
```

Construction uses a unique sibling directory ending in `.tmp-<nonce>`. Sorted
runs and the growing payload live only there. The builder writes and flushes all
page payloads, writes the complete checksummed manifest last, closes every file,
then atomically renames the directory to its final name. A cancellation, crash,
or write error can therefore leave only an ignored temporary directory. A
temporary directory is never considered a cache hit.

If another process wins the same commit race, the loser validates and reuses
the winner, then discards its temporary build. An invalid final entry is removed
only after validation fails and a complete replacement is ready to commit.

## Manifest encoding

`manifest.pci` is explicitly little-endian and consists of:

- eight-byte magic `PCIPGS01`;
- format version `1`, schema version `1`, and endian marker `0x01020304`;
- 32-byte SHA-256 source fingerprint;
- source size, modification timestamp, source point count, and source bounds;
- hierarchy depth, leaf-page target, root-preview target, and attribute flags;
- observed intensity/classification/return-number ranges;
- length-prefixed canonical path, PDAL driver, CRS WKT, and dimension names;
- payload byte size and a fixed-schema page table; and
- CRC-32 of every preceding manifest byte.

Unknown versions, a bad endian marker, impossible lengths/counts, non-finite or
invalid bounds, an absent root, out-of-range page coordinates, overlapping or
out-of-file payload extents, duplicate page IDs, and a bad manifest CRC all make
the entry invalid. There is no compatibility shim: an older/newer private cache
is rebuilt by the current executable.

Each page-table record stores the stable octree ID `(level,x,y,z)`, tight bounds,
source-descendant count, decoded point count, payload offset/size, and payload
CRC-32. Parent/child relationships are implicit in the stable octree ID.
Geometric error is deterministic from the source extent, level, and page target.

## Payload schema

`payload.bin` is a concatenation of uncompressed, independently checksummed
pages. Every point uses 34 bytes, explicitly little-endian:

```text
float64 x, y, z
uint32  rgba
uint16  packed classification/intensity preview
uint32  packed intensity/return-number/number-of-returns
```

The full CPU `PointAttributes` value is reconstructed from those packed fields;
it is not duplicated on disk. A requested page is read and validated alone,
then partitioned into immutable `PointBlock` uploads no larger than
`maximumPointsPerBlock`. V1 deliberately uses no extra compression dependency:
LAZ remains the compact source of record, while this cache optimizes bounded
random-page latency and simple recovery.

## Hierarchy and progressive construction

The uniform octree-shaped depth is selected from the inspected point count
using a 32K leaf limit. Points are sorted by a fine-grained Morton key and that
ordered stream is divided into fixed-size leaf chunks. Dense or duplicate
coordinates therefore cannot create an unbounded page, while adjacent chunks
retain spatial locality. Stable leaf IDs encode the chunk sequence and parent
IDs group each consecutive set of eight chunks. Stored tight subtree bounds,
rather than the ID alone, drive culling. During the only LAS/LAZ scan, the
builder:

1. accumulates a bounded spatial root preview and publishes it after at most
   65,536 visited points;
2. creates sorted Morton runs within a 64 MiB default sort allowance;
3. merges runs directly into leaf payloads capped at the configured leaf size
   without retaining a leaf in memory;
4. constructs parents bottom-up from at most eight child streams using bounded,
   deterministic reservoir sampling; and
5. appends a final bounded root sample from the completed top hierarchy and
   makes that record, rather than the source-order warm-up record, authoritative
   in the committed manifest; and
6. publishes each completed page to the live `LocalPointPageSource` before the
   final atomic commit.

The root descriptor exposes the final depth from the start. A page request made
while construction is active waits interruptibly for that page or completion;
the resident root remains drawable, so missing detail never creates a hole.
The completed manifest is the only metadata read on a cache hit. Reopening an
unchanged source reads the fingerprint samples, manifest, root page, and later
only explicitly requested payloads; it never scans every LAS/LAZ point.

Builder memory is bounded by the sort chunk, root preview, one parent reservoir,
run-reader state, and PDAL's documented stream table. The payload temporarily
contains both the early and final bounded roots; only the final record is
reachable from the committed manifest, and disk admission accounts for both.
Page-table metadata grows with occupied nodes and is measured separately from
decoded payload residency.

## Cancellation, corruption, and disk limits

All scan, sort, merge, parent-build, and wait boundaries observe a stop token.
Cancellation marks the live source failed (waking waiters), closes files, and
removes the temporary directory. It never publishes a committed entry.

Manifest corruption is detected at open and causes a rebuild. Payload corruption
is detected by that page's CRC before publication into the decoded cache. The
loader also validates the root before presenting a cache hit. Disk-full and
permission failures are ordinary isolated import errors; the prior valid entry,
if any, is not replaced.

The default page-cache allowance is 20 GiB. Before construction, the builder
estimates committed and peak temporary storage, prunes old entries to make room,
rejects a source that cannot fit the configured allowance, and checks currently
available filesystem space when the platform reports it. After a successful
commit the cache prunes least-recently-written valid entries until it is within
the allowance, never deleting an entry with a live process lease. Disk
accounting includes manifests, payloads, and temporary sort runs. There is no
compiled-in source-count limit.

## Versioning rules

Any change to point encoding, page-table fields, hierarchy construction,
checksum semantics, or validation rules increments the format or schema version
and the fingerprint schema input. Current code reads only the exact v1 pair.
Cache rebuilds are expected and must remain safe and automatic.

## Release H integration

Release G adapts v1 pages through the document-owned decoded cache, bounded
decode scheduler, source-bounds index, and global coverage/detail planner.
Release H exposes per-source lifecycle and recovery, cache/index disk use,
cache/scheduler/RSS diagnostics, and a parameterized local multi-file benchmark.
Cold-build, reopen, and native-GPU reports for the available representative
files are archived in `qualification/`. A different production dataset still
requires its own measurement run; there is no source-count constant. The page
store continues to avoid complete decoded flat vectors and cannot bypass the
document CPU budget.
