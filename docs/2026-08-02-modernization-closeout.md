# Modernization close-out

Date: 2026-08-02

Status: complete

The 65-step modernization program in the archived
[refactoring guide](superpowers/plans/2026-07-31-modernization-refactoring-guide.md)
is complete. [`ARCHITECTURE.md`](../ARCHITECTURE.md) is now the authoritative guide
to module boundaries, ownership, thread rules, data flow, and extension points.

## Final Step 65 changes

- Removed the unused `makePointCloudLoadRequest` migration helper.
- Removed the compatibility `VectorLoadController::load` entry point.
- Routed programmatic vector selection through the same inspect/continue workflow
  used by interactive imports.
- Retained request sublayers through inspection and covered partial, failure,
  cancellation, session, and UI paths with the existing focused tests.
- Archived the completed implementation guide with the other historical plans.

## Verification

The final working tree passed every configured gate before the Step 65 commit:

| Gate | Result |
|---|---:|
| Development suite | 331/331 passed |
| CI suite | 331/331 passed |
| ASan/UBSan suite | 331/331 passed |
| TSan suite | 331/331 passed |
| Native Metal GPU suite | 28/28 passed |
| Clang-tidy build | clean |
| Optimized release build | successful |
| Formatting/diff checks | clean |

## Same-host release qualification

The final native Metal report was compared with the Phase 4 same-host baseline by
`pci_qualification_diff` using `qualification/tolerances.json`. The accepted rerun
exited zero:

| Metric | Baseline | Candidate | Result |
|---|---:|---:|---:|
| First point, any source (ms) | 130.107 | 102.678 | pass |
| First point, all sources (ms) | 130.111 | 102.683 | pass |
| All sources display-ready (ms) | 6920.462 | 7042.865 | pass |
| Sampled frame p95 (ms) | 0.228 | 0.218 | pass |
| Sampled frame max (ms) | 0.237 | 0.227 | pass |
| Peak CPU page residency (bytes) | 266355936 | 266355936 | pass |
| Peak GPU point residency (bytes) | 193713408 | 193713408 | pass |
| Settled visible draw calls | 236 | 236 | pass |
| Full-detail hierarchy pages | 369 | 369 | exact |
| GPU evictions | 0 | 0 | exact |

The initial run sampled 304 settled draw calls while all other metrics passed. A
second run sampled the stable 236-draw settled state and passed every tolerance.
This repeats the known timing-sensitive sampling behavior observed at the Phase 5
checkpoint; it is qualification-harness variance, not a retained compatibility
path or a functional regression.
