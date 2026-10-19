#!/usr/bin/env bash
# Run inside the same KDE SDK image used by the GitHub Flatpak job.
set -euo pipefail
cd /work
reports=/reports
stage=environment
build_started=false

collect_diagnostics() {
    if [[ "$build_started" != true ]]; then return; fi
    while IFS= read -r -d '' file; do
        relative=${file#/cache/state/build/}
        mkdir -p "$reports/configuration/$(dirname "$relative")"
        cp "$file" "$reports/configuration/$relative"
    done < <(find /cache/state/build -type f \( -name CMakeCache.txt \
        -o -name compile_commands.json -o -name CMakeCXXCompiler.cmake \
        -o -name CMakeConfigureLog.yaml \) -print0)
    for testing in /cache/state/build/point-cloud-inspector-*/_flatpak_build/Testing; do
        if [[ -d "$testing" ]]; then
            mkdir -p "$reports/Testing"
            cp -R "$testing/." "$reports/Testing/"
            if [[ -f "$testing/ctest.xml" ]]; then
                cp "$testing/ctest.xml" "$reports/ctest.xml"
            fi
        fi
    done
}

finish() {
    result=$?
    trap - EXIT
    set +e
    collect_diagnostics
    if [[ "$stage" == process-check && "$result" != 0 ]]; then
        cat "$reports/process-check.log" >&2
        echo "Flatpak SDK process check failed (exit $result); application tests did not run." >&2
        if [[ "${PCI_CONTAINER_ARCH:-}" == amd64 ]]; then
            echo 'Rosetta can hang on macOS 15; use --arch arm64 for native checks. See scripts/README.md.' >&2
        fi
    fi
    if declare -F environment_info >/dev/null; then
        environment_info > "$reports/environment-final.log" 2>&1
    fi
    if [[ "$stage" == build && -f "$reports/ctest.xml" ]] && \
        python3 -c 'import sys, xml.etree.ElementTree as E; r=E.parse(sys.argv[1]); sys.exit(r.find(".//failure") is None and r.find(".//error") is None)' "$reports/ctest.xml"; then
        stage=test
    fi
    printf '{"stage":"%s","exit_code":%s}\n' "$stage" "$result" > "$reports/result.json"
    exit "$result"
}
trap finish EXIT

# Match the action's session services without installing anything on macOS.
if [[ "${PCI_FLATPAK_SESSION:-}" != 1 ]]; then
    exec dbus-run-session -- xvfb-run --auto-servernum \
        env PCI_FLATPAK_SESSION=1 bash "$0"
fi
mkdir -p /cache
exec 9>/cache/build.lock
flock -n 9 || { echo 'Flatpak cache is already in use by another run.' >&2; exit 1; }
umask 0022
manifest=packaging/flatpak/io.github.dwastberg.PointCloudInspector.yml
prepared=packaging/flatpak/local-test-manifest.json
flatpak-builder --show-manifest "$manifest" > "$reports/manifest.json"
python3 -c 'import json, sys; m=json.load(open(sys.argv[1])); print(m["sdk"], m["runtime"], m["runtime-version"], m.get("app-id") or m["id"], sep="\n")' \
    "$reports/manifest.json" > "$reports/runtime-settings.txt"
mapfile -t settings < "$reports/runtime-settings.txt"
arch=${PCI_FLATPAK_ARCH:?Missing Flatpak architecture}
sdk_ref="${settings[0]}/$arch/${settings[2]}"
runtime_ref="${settings[1]}/$arch/${settings[2]}"
app_id=${settings[3]}

run_stage() {
    stage=$1
    shift
    printf '\n>>> flatpak: %s\n' "$stage"
    "$@" 2>&1 | tee "$reports/$stage.log"
}

environment_info() {
    uname -a
    id
    flatpak --version
    flatpak-builder --version
    flatpak list --runtime --columns=application,branch,arch,active:full
    flatpak info --show-commit "$sdk_ref"
    flatpak info --show-commit "$runtime_ref"
}
run_stage environment environment_info
export PCI_CONTAINER_ARCH=${PCI_CONTAINER_ARCH:?Missing container architecture}
export FLATPAK_BUILDER_N_JOBS=${PCI_BUILD_JOBS:-4}
# A Rosetta-stalled descendant can outlive timeout and retain its stdout. Use
# a regular file here so an inherited pipe cannot keep tee (and us) waiting.
stage=process-check
printf '\n>>> flatpak: process-check\n'
timeout --kill-after=5s 30s flatpak run \
    --filesystem=/work:ro --env="PCI_CONTAINER_ARCH=$PCI_CONTAINER_ARCH" \
    --command=cmake "$sdk_ref" \
    -P /work/scripts/container/check-processes.cmake > "$reports/process-check.log" 2>&1
cat "$reports/process-check.log"

# The canonical JSON representation avoids a host-side YAML dependency. Keep
# the generated manifest next to the original so relative source paths work.
python3 scripts/container/prepare-flatpak-manifest.py "$reports/manifest.json" "$prepared"
cp "$prepared" "$reports/effective-manifest.json"
version=$(bash scripts/release.sh version)

# Dependency modules remain cached. Remove only the application's old build
# trees so failed setup/builds cannot pick up an earlier run's test results.
for previous in /cache/state/build/point-cloud-inspector-*; do
    if [[ -d "$previous" ]]; then rm -rf -- "$previous"; fi
done
build_started=true
run_stage build flatpak-builder --repo=/cache/repo --state-dir=/cache/state \
    --disable-rofiles-fuse --install-deps-from=flathub --force-clean \
    --default-branch=stable --arch="$arch" --keep-build-dirs --ccache \
    /cache/flatpak_app "$prepared"
collect_diagnostics
run_stage capability flatpak build /cache/flatpak_app /app/bin/pcinspector --gdal-capabilities
package="PointCloudInspector-$version-linux-$arch.flatpak"
run_stage bundle flatpak build-bundle --arch="$arch" \
    --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    /cache/repo "$reports/$package" "$app_id" stable
(cd "$reports" && sha256sum "$package" > "$package.sha256" && sha256sum --check "$package.sha256")
test -s "$reports/ctest.xml"
stage=complete
