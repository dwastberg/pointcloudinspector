# Local page-store v1 compatibility fixture

`store.pcipages` was generated on 2026-08-02 by the pre-step-61
`LocalPointIndexBuilder` at commit `6b451e6`, using the eight-point LAS fixture,
`pointsPerLeaf = 2`, `rootPreviewPoints = 2`, and `maximumPoints = 8`.
The fixture's product-specific magic and trailing CRC were rebased from the old
application abbreviation to `PCI` during the 2026-08-03 product rename.

The original source lived in a temporary test directory. Its canonical path is
intentionally retained in `manifest.pci`: the compatibility test supplies the
stored fingerprint directly and verifies that the v1 reader can open every
page without depending on that source file.

- `manifest.pci`: 1,642 bytes, SHA-256
  `1287d9de970ee542676b3f1e6a0d4c7ea9edb24e149827af6727a78a9b6c5f65`
- `payload.bin`: 408 bytes, SHA-256
  `81e5cc6dbf63a2303966063279def2f82826b439da3750d07d1a840c2c14a257`
- stored source fingerprint:
  `461035be96c5594450778cc1b4123afdd3e004a59b34d1bf265d80c22a7a0be7`
