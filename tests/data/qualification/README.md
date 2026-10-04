# Synthetic qualification-diff fixtures

These reports exercise `pci_qualification_diff` schema validation, workload
comparability, ignored metadata, and performance tolerances. They contain
invented deterministic values and paths, not measured acceptance baselines.

`headless.json` and `headless-reopen.json` cover multi-file reports;
`native.json` covers rendering reports. No GPU is needed to test these fixtures.
The Catch2 suite tests comparison behavior, and one CLI smoke test checks the
executable when developer tools are enabled.
