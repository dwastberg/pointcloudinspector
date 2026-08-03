# Phase 4 qualification record

This checkpoint records the verification required after Phase 4 of the
modernization guide. The comparison was run on 2026-08-02 between the
pre-Phase-4 commit `093e8af` and the Phase 4 candidate `2a43fc5`.

## Host and workload

- Apple M4 Pro, arm64, macOS 15.7.1 (24G231)
- Metal backend with GPU validation enabled
- 4 GiB CPU point-page budget and 512 MiB GPU point-buffer budget
- `test_data/3445-343.laz` and `test_data/3445-344.laz`
- 12,074,320 source points across two reused local indexes

Both reports were produced from fresh builds on the same host. The baseline was
built from a detached worktree at `093e8af`; the candidate was built from
`2a43fc5`. The reports were intentionally kept as local measurement artifacts
rather than portable repository baselines.

## Automated verification

| Lane | Result |
| --- | ---: |
| Development | 324/324 passed |
| CI warnings-as-errors | 324/324 passed |
| ASan/UBSan | 324/324 passed |
| TSan concurrency subset | 17/17 passed |
| Native Metal | 353/353 passed |

The first native run had two hierarchy tests miss their timing thresholds. Both
passed immediately when rerun in isolation, and a clean second run passed all
353 tests. No production or test-threshold change was made for the transient
timeouts.

## Same-host qualification comparison

`pci_qualification_diff` exited zero using `qualification/tolerances.json`.

| Measurement | Baseline | Candidate | Change | Result |
| --- | ---: | ---: | ---: | ---: |
| First point from any source | 195.622 ms | 130.107 ms | -33.49% | Pass |
| First point from all sources | 195.627 ms | 130.111 ms | -33.49% | Pass |
| All sources display-ready | 7,199.878 ms | 6,920.462 ms | -3.88% | Pass |
| Sampled frame p95 | 0.309 ms | 0.228 ms | -26.46% | Pass |
| Sampled frame maximum | 0.322 ms | 0.237 ms | -26.50% | Pass |
| Peak CPU page residency | 266,355,936 B | 266,355,936 B | 0% | Pass |
| Peak GPU point residency | 193,713,408 B | 193,713,408 B | 0% | Pass |
| Settled visible draw calls | 304 | 236 | -22.37% | Pass |
| Full-detail hierarchy pages | 369 | 369 | exact | Pass |
| GPU evictions | 0 | 0 | exact | Pass |

Phase 4 therefore satisfies its test, sanitizer, native-GPU, and same-host
qualification gates without a blocking regression.
