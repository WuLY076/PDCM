# PDCM testkit

`testkit/` contains GitHub-only test infrastructure, including MockProvider,
mock catalogs, deterministic scenarios, fixtures, and end-to-end test hosts.

This directory is excluded from the future GitLab production-code export.
Production code under `product/` must never include headers from or link
targets defined in this directory.

The current test namespace uses IDs in the `0xF0000000` range. Those IDs are
not part of the public ABI and must never appear in the production catalog.
`mock_p0.yaml` exercises raw gauges, cumulative counters, derived metrics,
partial failures, and firmware-heartbeat evidence without claiming target
support.
