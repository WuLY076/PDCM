# Public API reference

Include `<pdcm/pdcm.h>` from C11 or C++. Public structures start with
`pdcm_struct_header_t`; initialize them with the supplied `*_INIT` macro.
Opaque handles are closed with `pdcm_close`.

Exported ABI 0.1 symbols:

- `pdcm_open`
- `pdcm_version_get`
- `pdcm_entity_list`
- `pdcm_capability_query`
- `pdcm_health_query`
- `pdcm_close`

`pdcm_open` selects standalone, embedded, or explicitly authorized automatic
fallback mode. Standalone uses a bounded local Unix-domain protocol.
`pdcm_version_get(NULL, ...)` returns local library information without a
daemon.

`pdcm_entity_list` uses a count-then-fill contract. It returns zero or one
Device in P0. `pdcm_capability_query` similarly reports required item count
before filling caller-owned storage.

`pdcm_health_query` accepts a Device reference, firmware-heartbeat subsystem,
optional catalog generation, and optional maximum evidence age. A successful
or partial response fills state, item status, stable code, generation, timing,
evidence identity/source, and the first bounded limitation.

The library never accepts a Provider library path. PDRL types, handles, symbols,
and native error objects are not part of the public ABI. Exceptions never cross
the C boundary.

Thread safety is per opaque handle; close waits for active public calls.
Deadlines are monotonic. A stale entity or catalog reference returns
`PDCM_STATUS_STALE_GENERATION`.
