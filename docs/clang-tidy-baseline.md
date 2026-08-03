# clang-tidy gate and baseline

This baseline was recorded on 2026-07-31 with Homebrew LLVM clang-tidy
22.1.8 and AppleClang 17 compile commands on macOS. It was promoted to a CI gate
on 2026-08-02: every enabled diagnostic now fails the build.

The configured project-only checks are in [`.clang-tidy`](../.clang-tidy).
Vendored, generated, build-tree, and system headers are excluded from the
baseline. Configure and run the lane with:

```sh
cmake --preset clang-tidy
cmake --build --preset clang-tidy --parallel
```

The initial run completed successfully. After de-duplicating identical
project location/message/check diagnostics across translation units, it
reported 252 findings:

| Check | Findings |
|---|---:|
| `bugprone-easily-swappable-parameters` | 149 |
| `readability-braces-around-statements` | 33 |
| `performance-enum-size` | 25 |
| `bugprone-branch-clone` | 14 |
| `bugprone-empty-catch` | 11 |
| `modernize-use-equals-default` | 8 |
| `bugprone-infinite-loop` | 4 |
| `bugprone-narrowing-conversions` | 3 |
| `performance-unnecessary-value-param` | 3 |
| `bugprone-std-namespace-modification` | 1 |
| `performance-no-automatic-move` | 1 |

The broad baseline was triaged before promotion. Checks that primarily request
API/ABI redesign (`bugprone-easily-swappable-parameters`,
`performance-enum-size`), conflict with deliberate value/ownership semantics
(`performance-unnecessary-value-param`), or produce framework-unaware findings
across Qt/Catch control flow (`bugprone-exception-escape`,
`bugprone-unchecked-optional-access`) are explicitly excluded in `.clang-tidy`.
`bugprone-empty-catch` is excluded because the reviewed sites are documented
best-effort cleanup or task/error containment boundaries.
Widening and pointer-order heuristics are also excluded until each domain can
state the intended conversion/order contract. The established unbraced control
flow style is owned by clang-format rather than clang-tidy.

All other configured bugprone, performance, modernization, and focused
readability checks are gating. Toolchain upgrades require a fresh clean run
because check behavior can change.
