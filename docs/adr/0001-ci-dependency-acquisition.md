# ADR 0001: Bootstrap CI with the qualified macOS dependency stack

Date: 2026-07-31

Status: accepted for the initial CI gate

## Context

Point Cloud Inspector requires Qt 6.7 or newer with `GuiPrivate` and `ShaderTools`, PDAL
2.10 or newer, and GDAL 3.4 or newer. The only environment qualified before CI
was introduced is macOS 15 on Apple Silicon using the Homebrew Qt, PDAL, and
GDAL formulae. QRhi's private Qt API also requires the runtime and private
headers to come from the same Qt build.

Building Qt, PDAL, and GDAL from a new package-manager manifest would add a
large unqualified dependency migration to the CI bootstrap. A container cannot
represent the qualified Metal/macOS toolchain, although native-GPU tests remain
manual and are not run by this workflow.

## Decision

The required workflow runs the warnings-as-errors `ci` preset, the complete
non-GPU suite, fail-on-diagnostic clang-tidy, ASan/UBSan, and TSan on GitHub's
pinned `macos-15` runner. It installs `qt`, `pdal`, `gdal`, and the analysis tools
from Homebrew and exports the resolved dependency prefixes to CMake. CMake
continues to enforce the repository's declared minimum package versions.

The workflow uses one runner family deliberately. The README must not claim
automated Windows or Linux validation until those lanes exist. The Windows
lane is required before the platform-path migration in the modernization guide
can be considered fully qualified.

## Consequences

- Pull requests receive warnings-as-errors, formatting, dependency-boundary,
  static-analysis, sanitizer, and complete non-GPU test gates on the previously
  qualified platform.
- The runner image is pinned, but Homebrew formula revisions are not bitwise
  pinned. A formula update can therefore expose compatibility problems. This is
  accepted for the bootstrap and must be revisited if update churn makes CI
  unreliable.
- A future dependency lock through vcpkg, Conan, or a maintained binary cache
  must first prove that it supplies the matching Qt private targets and remains
  practical within hosted-runner time limits.
- Native-GPU and performance qualification remain required developer lanes.
