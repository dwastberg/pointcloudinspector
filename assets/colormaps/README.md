# Embedded color maps

Every `.cpt` file in this directory and its subdirectories is embedded into
the application at build time and parsed during startup. Add
`# PCINSPECTOR_NAME = Display name` to override the filename-derived UI name.

The loader accepts regular GMT CPT intervals with RGB, grayscale, HSV, or CMYK
endpoint colors. It normalizes the CPT numeric domain to `[0, 1]` and makes
the map available for X, Y, Z, and intensity coloring. Discontinuous,
piecewise-constant intervals are preserved. Categorical, cyclic, pattern-fill,
and `+HSV` interpolation palettes are not currently supported.

No third-party CPT files are bundled with the application. Before adding one,
verify that its license is compatible with redistribution under the project's
open-source terms, record its provenance here, and keep any required
attribution in the file header.
