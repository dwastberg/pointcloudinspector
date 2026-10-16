#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "$*" >&2; exit 1; }

version() {
    local value expected=${1:-} file=${2:-}
    value=$(sed -n '/^[[:space:]]*project(pcinspector/,/)/{
        s/.*VERSION[[:space:]][[:space:]]*\([^[:space:])]*\).*/\1/p
    }' CMakeLists.txt)
    [[ "$value" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || fail "Invalid CMake project version: $value"
    [[ -z "$expected" || "$expected" == "$value" ]] || fail "Expected version $expected, found $value"
    if [[ -n "$file" ]]; then
        [[ -f "$file" ]] || fail "Missing configured version file: $file"
        [[ "$(tr -d '\r\n' < "$file")" == "$value" ]] || fail "Configured version differs from $value"
    fi
    printf '%s\n' "$value"
}

case ${1:-} in
    version)
        [[ $# -le 3 ]] || fail "Usage: release.sh version [expected-version] [configured-version-file]"
        version "${2:-}" "${3:-}"
        ;;
    publish)
        [[ $# -eq 4 ]] || fail "Usage: release.sh publish <tag> <repository> <artifact-directory>"
        tag=$2 repository=$3 directory=$4
        value=$(version "${tag#v}")
        [[ "$tag" == "v$value" ]] || fail "Release tag must be v$value"

        if command -v sha256sum >/dev/null 2>&1; then
            hasher=(sha256sum)
        else
            hasher=(shasum -a 256)
        fi
        suffixes=(windows-x64.msi macOS-arm64.dmg linux-x86_64.flatpak)
        platforms=("Windows x64" "macOS Apple silicon" "Linux x86_64")
        assets=()
        notes=$(mktemp)
        trap 'rm -f "$notes"' EXIT
        printf '## Downloads\n\n| Platform | Download | SHA-256 |\n|---|---|---|\n' > "$notes"
        for i in "${!suffixes[@]}"; do
            name="PointCloudInspector-$value-${suffixes[$i]}"
            package="$directory/$name"
            [[ -f "$package" && -s "$package" ]] || fail "Missing or empty installer: $package"
            [[ -f "$package.sha256" && -s "$package.sha256" ]] || fail "Missing or empty checksum: $package.sha256"
            # CPack writes a bare digest; shasum and sha256sum include a filename.
            expected=$(tr -d '\r' < "$package.sha256" | awk 'NR == 1 {print tolower($1)}')
            actual=$("${hasher[@]}" "$package" | awk '{print $1}')
            [[ "$expected" =~ ^[[:xdigit:]]{64}$ && "$actual" == "$expected" ]] || fail "SHA-256 mismatch: $name"
            url="https://github.com/$repository/releases/download/$tag/$name"
            printf '| %s | [Download](%s) | [Checksum](%s.sha256) |\n' "${platforms[$i]}" "$url" "$url" >> "$notes"
            assets+=("$package#${platforms[$i]}" "$package.sha256#${platforms[$i]} SHA-256")
        done
        printf '\nWindows is unsigned. The macOS app is ad-hoc signed without Developer ID signing or notarization.\n' >> "$notes"
        if gh release view "$tag" --repo "$repository" >/dev/null 2>&1; then
            fail "Release $tag already exists; refusing to replace it"
        fi
        gh release create "$tag" "${assets[@]}" --repo "$repository" --verify-tag \
            --title "Point Cloud Inspector $value" --generate-notes --notes-file "$notes"
        ;;
    *) fail "Usage: release.sh {version|publish} ..." ;;
esac
