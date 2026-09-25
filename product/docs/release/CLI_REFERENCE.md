# CLI reference

`pdcm-cli` uses only the public `libpdcm.so` API. It defaults to standalone
mode and never starts an embedded service implicitly.

## Commands

- `pdcm-cli help` and `pdcm-cli --help`: local help.
- `pdcm-cli version`: library, protocol, daemon, Core, and Provider state.
- `pdcm-cli discovery`: zero-or-one Device inventory.
- `pdcm-cli dmon`: approved P0 metrics; currently returns
  `CATALOG_BLOCKED_EXTERNAL`.
- `pdcm-cli health`: firmware-heartbeat Health.

Global options are `--format human|json`, `--timeout 1ms..60s`,
`--endpoint ABSOLUTE_PATH`, `--mode standalone|embedded`, `--device ID`,
and `--verbose`. Health accepts `--fresh`. Dmon additionally accepts
repeatable `--metric`, `--count`, `--period`, and `--fresh`. In the generic
build, Health `--fresh` enforces a near-zero maximum cache age; an active
target Provider read remains unavailable until the PDRL mapping is approved.

P0 has no `--all`, `diag`, `reset`, configuration mutation, operation,
policy, profile, stats, topology, module, or exporter command.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | complete success |
| 1 | completed with a partial item or WARNING/ERROR/UNKNOWN Health |
| 2 | usage or argument error |
| 3 | unsupported capability/topology or blocked catalog |
| 4 | permission denied |
| 5 | daemon, Provider, PDRL, or Device unavailable |
| 6 | timeout or resource exhaustion |
| 7 | protocol-major incompatibility |
| 8 | internal or serialization failure |

JSON output uses schema version 1 and stable English enum values. The envelope
contains command status, request and catalog identity, Core/Provider state,
items, and errors. JSON mode uses the same exit code as human mode.

Examples:

```sh
pdcm-cli version --format json
pdcm-cli discovery --device 0
pdcm-cli health --fresh --timeout 2s
pdcm-cli dmon
```
