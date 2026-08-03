# Release H local-file qualification

These reports are reproducible evidence for the Release H implementation. They
are measurements, not compiled-in capacity targets. Source count and point
count remain constrained by the configured CPU, GPU, import, and disk budgets.

## Workload and host

The repository's available representative workload contains two local LAZ
files: 4,538,547 and 7,535,773 points (12,074,320 total). Their compressed
source size is 50,790,725 bytes. The persistent v1 local indexes occupy
441,771,038 bytes.

Native qualification ran on an Apple M4 Pro, arm64, macOS 15.7.1, using Metal
with GPU validation, a 4 GiB CPU point-page budget, and a 512 MiB GPU point
budget. The final report is
[`release-h-native-metal-final.json`](release-h-native-metal-final.json).

These stored numbers are historical evidence, not a portable performance
baseline. Cross-host comparison is meaningless: compare a fresh local baseline
with a fresh local candidate on the same machine, using the same backend,
validation mode, budgets, and workload. The committed tolerance table is
[`tolerances.json`](tolerances.json); its starting values must be tuned from
same-host repeat runs before being treated as release thresholds.

Measured results:

| Measurement | Result |
| --- | ---: |
| First point from any/all sources | 164.0 / 176.5 ms |
| All sources display-ready | 7,139.3 ms |
| Full-detail hierarchy pages | 369 / 369 |
| CPU page residency, peak/final | 266,355,936 bytes |
| GPU point residency, peak/final | 193,713,408 bytes |
| Process RSS peak/final | 915,095,552 bytes |
| GPU evictions | 0 |
| Settled visible draw calls | 368 |
| Sampled frame p50 / p95 / max | 0.371 / 0.460 / 0.590 ms |

The renderer retained all hierarchy pages and made an atomic coarse-to-full
cutover. `submitted_points` is the frustum-visible subset in the settled frame,
not a residency count; all 369 full-detail pages were resident. Measurements
did not identify buffer creation or command recording as a material bottleneck,
so Release H deliberately does not add a GPU arena or a new draw-batching layer.

## Headless import, cache reuse, and scheduler qualification

[`release-h-local-two-file.json`](release-h-local-two-file.json) records a cold
two-file local-index build. First previews arrived in 155.7/160.7 ms, import and
indexing completed in 28.26 seconds, peak RSS was 316,932,096 bytes, and the
document remained within its CPU residency budget.

[`release-h-local-two-file-reopen.json`](release-h-local-two-file-reopen.json)
records the same workload after restart. Both indexes were reused, first
previews arrived in 86.5/86.8 ms, and import completed in 86.9 ms with peak RSS
of 62,701,568 bytes.

Reproduce the headless measurement with:

```sh
build/development/tools/pci_multifile_bench \
  --cpu-cache-mb 4096 \
  --cache-dir qualification/page-cache \
  --json qualification/release-h-local-two-file.json \
  test_data/3445-343.laz test_data/3445-344.laz
```

Reproduce the native report with:

```sh
build/development/src/pcinspector.app/Contents/MacOS/pcinspector \
  --cpu-cache-mb 4096 --gpu-cache-mb 512 \
  --graphics-api metal --gpu-validation \
  --qualification-report qualification/release-h-native-metal-final.json \
  --qualification-exit \
  test_data/3445-343.laz test_data/3445-344.laz
```

Compare two same-host reports with:

```sh
build/development/tools/pci_qualification_diff \
  qualification/local-baseline.json \
  qualification/local-candidate.json \
  qualification/tolerances.json
```

## Scope of the evidence

Generated tests exercise 25-source fair coverage, low-budget residency,
hide/show/remove churn, cancellation, restart/index reuse, source invalidation,
cache corruption, and disk-admission failure. Those tests prove behavioral
contracts and that no fixed source-count limit exists. They do not substitute
for allocator and I/O measurements on an organization's own large production
dataset. The earlier “25 files at roughly 7 million points” scenario remains a
comparison workload, not a target or hard-coded ceiling; production sign-off
for such a dataset requires running the commands above on those files and
archiving the resulting reports.
