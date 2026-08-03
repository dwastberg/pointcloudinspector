# ADR 0002: Adopt clang-format before architectural refactoring

Date: 2026-07-31

Status: accepted

## Context

The modernization program will move and split most major C++ components. Adding
automatic formatting after those moves would mix mechanical changes into semantic
reviews and make regression attribution harder. The repository had no root
`.clang-format`; only vendored Catch2 carried its own configuration.

## Decision

Adopt a root clang-format configuration before any production source is moved. The
configuration preserves the project's established four-space indentation, attached
control/class braces, separate-line function-definition braces, right-aligned
pointer declarators, 80-column target, and unindented namespace bodies.

The `format` and `format-check` CMake targets operate only on project-owned C++ in
`main.cpp`, `src/`, `tests/`, and `tools/`. Vendored dependencies, generated build
trees, shaders, and generated Qt resources are excluded. CI installs clang-format
and makes `format-check` a required gate.

The initial mechanical rewrite is a dedicated commit. Later commits must not combine
repository-wide formatting with interface or behavior changes.

## Consequences

- Contributors can run `cmake --build --preset development --target format` and
  `format-check` using the formatter found at configure time.
- Machines without clang-format can still configure and build, but the formatting
  targets are unavailable and CI remains authoritative.
- Formatter releases can make different layout decisions despite a stable style
  file. If this causes churn, CI must pin a formatter distribution before accepting
  another whole-tree rewrite.
