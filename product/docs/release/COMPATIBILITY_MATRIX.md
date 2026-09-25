# Compatibility matrix

Matrix version: `UNFROZEN-TDD08`.

| PDCM | Target | PDRL ABI/SONAME | PDRV/PFW/HW | Status |
|---|---|---|---|---|
| 0.1.0 | FPGA | not frozen | not frozen | BLOCKED_EXTERNAL |
| 0.1.0 | EMU | not frozen | not frozen | BLOCKED_EXTERNAL |

The public PDCM ABI major is 0 and the local protocol major is 1. A protocol
major mismatch fails fast; compatible minor negotiation selects the lower
supported minor.

This placeholder does not authorize a PDRL path, SONAME, bootstrap symbol, ABI
minor rule, native structure, or target combination. Those values must come
from the approved TDD-08 compatibility artifact.

PDRL absence does not prevent package installation. At runtime it produces a
diagnosable degraded daemon and unavailable Provider-backed operations. No
target combination may be marked supported until its exact PDCM, PDRL, PDRV,
PFW, hardware, and catalog versions have passed target qualification.
