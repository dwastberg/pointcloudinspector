# Sanitizer baseline

This baseline was recorded on 2026-07-31 before the architectural refactoring
phases. It makes later sanitizer failures attributable to the change that
introduced them.

## Environment

- macOS 15.7.1 on Apple M4 Pro (arm64)
- AppleClang 17.0.0.17000404
- Debug configuration with tests and developer tools enabled
- Native-GPU tests disabled

## ASan and UBSan

Commands:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel
ctest --preset asan-ubsan
```

Result: all 253 non-GPU tests passed. AddressSanitizer and
UndefinedBehaviorSanitizer reported no findings. The test preset uses
`halt_on_error=1`; UBSan also prints a stack trace on failure.

## TSan

Commands:

```sh
cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan
```

The initial result was 50 focused concurrency and lifetime tests. On 2026-08-02
the preset was broadened to the complete non-GPU suite and promoted to CI. The
preset uses `halt_on_error=1` so a detected race fails immediately. The
promotion run passed all 331 tests under both ASan/UBSan and TSan with no
sanitizer findings.

The first baseline run found cross-thread reads of `QPointer` state in the
point-cloud and vector load controllers while QObject destruction could update
that state on the owner thread. The controllers now publish completions through
the shared `QueuedControllerCallback` boundary: standard mutex-protected queues
carry callback state between threads, and a captureless Qt event only wakes the
controller thread. The 50-test TSan subset passed after that fix without a
suppression.

## Scope and policy

The sanitizer presets intentionally exclude native-GPU tests because the
supported combinations of QRhi backend, validation layer, and sanitizer are
platform-specific. They also keep sanitizer modes mutually exclusive: ASan
cannot be combined with TSan.

Both sanitizer presets now gate every change and run every supported non-GPU
test. Update this document only when the suite scope or supported toolchain
changes; do not weaken the presets to hide a project finding.
