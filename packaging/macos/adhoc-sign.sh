#!/usr/bin/env bash

set -euo pipefail

app=${1:?Usage: adhoc-sign.sh /path/to/Application.app}

if [[ ! -d "$app/Contents" ]]; then
    echo "Application bundle not found: $app" >&2
    exit 1
fi

sign_payload()
{
    local target=$1
    local output
    if ! output=$(codesign --force --sign - "$target" 2>&1); then
        printf '%s\n' "$output" >&2
        return 1
    fi
}

# Dependency deployment modifies and adds binaries after Qt's deployment step.
# Sign every real Mach-O file first so containers can be signed afterwards.
while IFS= read -r -d '' candidate; do
    if file -b "$candidate" | grep -q 'Mach-O'; then
        sign_payload "$candidate"
    fi
done < <(find "$app/Contents" -type f -print0)

# A framework's signature covers its versioned binary and resources. Sign the
# framework containers after their contents, deepest paths first.
if [[ -d "$app/Contents/Frameworks" ]]; then
    while IFS= read -r framework; do
        sign_payload "$framework"
    done < <(
        find "$app/Contents/Frameworks" -type d -name '*.framework' -print |
            awk '{ print length, $0 }' |
            sort -rn |
            cut -d' ' -f2-
    )
fi

sign_payload "$app"
codesign --verify --deep --strict "$app"
