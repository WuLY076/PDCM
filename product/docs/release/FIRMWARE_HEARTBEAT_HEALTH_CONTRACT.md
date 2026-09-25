# Firmware heartbeat Health contract

P0 exposes exactly one subsystem:
`PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT`.

| Evidence | State | Stable code |
|---|---|---|
| fresh NORMAL | HEALTHY | HEARTBEAT_OK |
| fresh WARNING | WARNING | HEARTBEAT_WARNING |
| fresh FAULT | ERROR | HEARTBEAT_FAULT |
| missing | UNKNOWN | HEARTBEAT_MISSING |
| stale | UNKNOWN | HEARTBEAT_STALE |
| timeout | UNKNOWN | HEARTBEAT_TIMEOUT |
| read error | UNKNOWN | HEARTBEAT_READ_ERROR |
| unsupported mapping | UNKNOWN | HEARTBEAT_UNSUPPORTED |
| Provider unavailable | UNKNOWN | PROVIDER_UNAVAILABLE |

A result includes entity and catalog generation, item status, evaluation time,
evidence age, stable code, source when evidence exists, and an explanatory
limitation when it does not. Timeout, missing evidence, and read failures are
never reclassified as hardware ERROR. Only explicit fresh FAULT evidence
produces ERROR.

Other Health subsystems are unsupported in P0. Driver readiness, Device
access, PCIe, memory, sensors, RAS, and aggregate diagnostics are not declared
as Health. The native heartbeat mapping remains `BLOCKED_EXTERNAL` pending
TDD-08 inputs.
