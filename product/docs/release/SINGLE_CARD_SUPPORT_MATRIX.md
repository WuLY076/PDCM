# Single-card support matrix

| Target | 0 Device | 1 Device | 2+ Devices | Production data |
|---|---|---|---|---|
| FPGA | supported | supported | unsupported topology | blocked on PDRL |
| EMU | supported | supported | unsupported topology | blocked on PDRL |

P0 manages at most one Device per node. Discovery of multiple Devices returns
the detected count but no partially managed entity list. A caller cannot use a
Device selector to bypass that rule.

For one Device, baseline inventory contains PDCM ID, generation, native ID, PCI
BDF, state, and PDRV version. Conditional fields carry their own observation
status instead of a fabricated value.

Supported public capabilities in the current generic build are:

- single-card discovery and capability reporting;
- firmware-heartbeat Health semantics when an approved Provider mapping is
  present;
- a blocked production Metrics Catalog.

Target qualification remains `BLOCKED_EXTERNAL` until the PDRL contract,
native heartbeat mapping, Golden Inventory, and compatibility combinations are
frozen.
