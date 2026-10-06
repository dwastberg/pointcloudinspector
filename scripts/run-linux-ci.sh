#!/usr/bin/env bash
# Shared by GitHub Actions and the local Linux container. Activate the matching
# micromamba environment before invoking this script.
set -euo pipefail

preset=${1:?Usage: run-linux-ci.sh <ci|clang-tidy|asan-ubsan|tsan>}
case "$preset" in ci|clang-tidy|asan-ubsan|tsan) ;; *) echo "Unknown preset: $preset" >&2; exit 2 ;; esac
cd "$(dirname "$0")/.."
reports="$PWD/build/$preset/Testing"
mkdir -p "$reports"
stage=environment
finish() {
    result=$?
    trap - EXIT
    printf '{"stage":"%s","exit_code":%s}\n' "$stage" "$result" > "$reports/result.json"
    exit "$result"
}
trap finish EXIT

run_stage() {
    stage=$1
    shift
    printf '{"stage":"%s","exit_code":null}\n' "$stage" > "$reports/result.json"
    printf '\n>>> %s: %s\n' "$preset" "$stage"
    "$@" 2>&1 | tee "$reports/$stage.log"
}

# TSan address layout is established by the CI sysctl step or the container
# entrypoint. Some Linux kernels restrict even reading this setting to root.
run_stage configure cmake --preset "$preset" -G Ninja \
    "-DPCINSPECTOR_ENABLE_LONG_STRESS_TESTS=${PCINSPECTOR_LONG_STRESS:-OFF}"
run_stage build cmake --build --preset "$preset" --parallel 3
if [[ "$preset" == ci ]]; then
    run_stage format cmake --build --preset ci --target format-check
fi
run_stage test ctest --preset "$preset" --output-on-failure --no-tests=error \
    --output-junit "$reports/ctest.xml"
stage=complete
