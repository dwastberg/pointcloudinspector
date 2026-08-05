# Embedded color maps

Every `.cpt` file in this directory and its subdirectories is embedded into
the application at build time and parsed during startup. Add
`# PCINSPECTOR_NAME = Display name` to override the filename-derived UI name.

The loader accepts regular GMT CPT intervals with RGB, grayscale, HSV, or CMYK
endpoint colors. It normalizes the CPT numeric domain to `[0, 1]` and makes
the map available for X, Y, Z, and intensity coloring. Discontinuous,
piecewise-constant intervals are preserved. Categorical, cyclic, pattern-fill,
and `+HSV` interpolation palettes are not currently supported.

Before adding a third-party CPT, verify that its license is compatible with
redistribution under the project's open-source terms, record its provenance
here, and keep any required attribution in the file header.

## Third-party palettes

### Oleron Land

`oleron-land.cpt` is a land-only derivative of Fabio Crameri's SCM/oleron
topography colour map. The negative bathymetry half was removed; the positive
intervals and RGB values are unchanged. It is based on Scientific Colour Maps
8.0.1 as distributed in GMT's `share/cpt/SCM/oleron.cpt`:

https://github.com/GenericMappingTools/gmt/blob/master/share/cpt/SCM/oleron.cpt

Copyright (c) 2023, Fabio Crameri. Licensed under the MIT License:

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
