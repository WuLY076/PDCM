# P0 Metric Catalog

Status: `BLOCKED_EXTERNAL`.

No production P0 Metric is approved in version 0.1.0. The installed FPGA and
EMU catalogs therefore contain an empty `metrics` list and advertise
`metrics_status: BLOCKED_EXTERNAL`.

The CLI returns `CATALOG_BLOCKED_EXTERNAL` with exit code 3 instead of
inventing names, types, units, periods, freshness, values, or Provider
mappings. This state cannot satisfy AC-3 and blocks a target release candidate.

Unblocking requires a jointly approved, versioned catalog that freezes, for
each target and Required Metric, its stable ID, name, type, unit, scope,
temporality, minimum/default period, freshness, semantic version, and PDRL
mapping. The same artifact must drive help, capability reporting, collection,
tests, and this document.

Test-only catalogs are not production catalogs and are never installed.
