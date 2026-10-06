#!/usr/bin/env bash
set -euo pipefail
preset=${1:?Missing CI preset}
case "$preset" in
    ci) environment=pcinspector-ci ;;
    clang-tidy|asan-ubsan|tsan) environment=pcinspector-validation ;;
    *) echo "Unknown preset: $preset" >&2; exit 2 ;;
esac

# Only the source archive and report directory are shared with macOS. Build on
# the guest filesystem, so case sensitivity and permissions match Linux CI.
finish() {
    result=$?
    trap - EXIT
    if [[ ! -f /reports/result.json ]]; then
        printf '{"stage":"environment","exit_code":%s}\n' "$result" > /reports/result.json
    fi
    exit "$result"
}
trap finish EXIT
if [[ "$preset" == tsan && $(sysctl -n vm.mmap_rnd_bits) != 28 ]]; then
    echo 'TSan requires vm.mmap_rnd_bits=28 (as configured by Linux CI).' >&2
    exit 1
fi
tar -xf /input/source.tar -C /work --no-same-owner
chown -R builder:builder /work
# Write diagnostics directly to the report mount so they survive a terminated
# VM. Only diagnostics use this mount; compilation stays on the Linux disk.
mkdir -p "/work/build/$preset"
ln -s /reports "/work/build/$preset/Testing"
chown -R builder:builder /work/build
cd /work
{
    echo "Container architecture: ${PCI_CONTAINER_ARCH:?Missing container architecture}"
    uname -a
    cat /etc/os-release
    cat /proc/cmdline
    sysctl vm.mmap_rnd_bits
    micromamba --version
    micromamba list -n "$environment" --explicit
} > /reports/environment.txt 2>&1

# Preserve activation hooks (compiler, sysroot, Qt paths) just as CI does.
sudo --preserve-env=MAMBA_ROOT_PREFIX,PCINSPECTOR_LONG_STRESS,PCI_CONTAINER_ARCH -H -u builder \
    bash -c '
        eval "$(micromamba shell hook --shell bash)"
        micromamba activate "$1" || exit 1
        # The sudo user-group policy can select 0002. Match CI checkout/build
        # permissions so private-cache ancestry is not group-writable.
        umask 0022
        cmake --version
        "${CXX:-c++}" --version
        timeout --kill-after=5s 30s cmake -P scripts/container/check-processes.cmake > /reports/process-check.log 2>&1
        result=$?
        cat /reports/process-check.log
        if [[ $result != 0 ]]; then
            echo "Environment check failed (exit $result): CMake could not reliably run child processes."
            if [[ $PCI_CONTAINER_ARCH == amd64 ]]; then
                echo "On macOS 15, Rosetta can hang with a pending SIGCHLD and a defunct child."
                echo "See https://github.com/libuv/libuv/issues/4279 and scripts/README.md."
            else
                echo "Native ARM execution failed; see process-check.log and scripts/README.md."
            fi
            exit "$result"
        fi
        exec bash scripts/run-linux-ci.sh "$2"
    ' bash "$environment" "$preset"
