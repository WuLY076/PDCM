# Known issues

Version 0.1.0 has these intentional external blockers:

- the production FPGA/EMU P0 Metric Catalog is not frozen;
- the PDRL loader ABI, trusted location, SONAME, and error mapping are not
  frozen;
- native Discovery and firmware-heartbeat mappings are not frozen;
- FPGA/EMU Golden Inventory, Golden heartbeat source, and platform lifecycle
  procedures are unavailable;
- target compatibility combinations are not qualified.

Consequently AC-3, target AC-4/AC-7, and complete AC-11 release qualification
remain `BLOCKED_EXTERNAL`.

Health `--fresh` currently applies a near-zero maximum age to cached evidence.
It cannot initiate an active target Provider read until the production PDRL
loader and native heartbeat mapping are available.

P0 supports only zero or one Device. Diagnostics, fault injection, reset,
configuration mutation, multi-card operation, and performance feature
qualification are outside scope. Dmon returns an explicit blocked-catalog
result until the approved catalog exists.
