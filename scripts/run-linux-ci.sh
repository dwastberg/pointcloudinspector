#!/usr/bin/env bash
# Shared by GitHub Actions and the local Linux container. Activate the matching
# micromamba environment before invoking this script.
set -euo pipefail

preset=${1:?Usage: run-linux-ci.sh <ci|ci-release|clang-tidy|asan-ubsan|tsan>}
case "$preset" in ci|ci-release|clang-tidy|asan-ubsan|tsan) ;; *) echo "Unknown preset: $preset" >&2; exit 2 ;; esac
cd "$(dirname "$0")/.."
reports="$PWD/build/$preset/Testing"
mkdir -p "$reports"
stage=environment
finish() {
    result=$?
    trap - EXIT
    # Keep targeted build metadata in the report mount even after a failed
    # configure or build. Diagnostic copying must preserve the original status.
    set +e
    for file in "build/$preset/CMakeCache.txt" "build/$preset/compile_commands.json" \
        "build/$preset/CMakeFiles/CMakeConfigureLog.yaml" \
        "build/$preset"/CMakeFiles/*/CMakeCXXCompiler.cmake; do
        if [[ -f "$file" ]]; then
            relative=${file#"build/$preset/"}
            mkdir -p "$reports/configuration/$(dirname "$relative")"
            cp "$file" "$reports/configuration/$relative"
        fi
    done
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

environment_info() {
    uname -a
    cmake --version
    "${CXX:-c++}" --version
    printf 'CFLAGS=%s\nCXXFLAGS=%s\nLDFLAGS=%s\n' "${CFLAGS-}" "${CXXFLAGS-}" "${LDFLAGS-}"
    if command -v micromamba >/dev/null 2>&1; then
        micromamba list --explicit > "$reports/packages.txt"
    fi
}

run_stage environment environment_info
# TSan address layout is established by the CI sysctl step or the container
# entrypoint. Some Linux kernels restrict even reading this setting to root.
run_stage configure cmake --preset "$preset" -G Ninja \
    "-DPCINSPECTOR_ENABLE_LONG_STRESS_TESTS=${PCINSPECTOR_LONG_STRESS:-OFF}"
if [[ "$preset" == ci || "$preset" == ci-release ]]; then
    run_stage compiler cmake "-DBUILD_DIR=$PWD/build/$preset" -P scripts/check-linux-ci-compiler.cmake
fi
run_stage build cmake --build --preset "$preset" --parallel 3
if [[ "$preset" == ci ]]; then
    run_stage format cmake --build --preset ci --target format-check
fi
run_stage test ctest --preset "$preset" --output-on-failure --no-tests=error \
    --output-junit "$reports/ctest.xml"
stage=complete
