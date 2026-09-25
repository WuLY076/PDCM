# PDCM 0.1.0 release notes

Status: development baseline; target release is `BLOCKED_EXTERNAL`.

This baseline provides:

- bounded Core lifecycle and diagnosable degraded startup;
- public C ABI with standalone and embedded backends;
- version, single-card discovery, capability, and firmware-heartbeat Health
  queries;
- Unix-domain IPC with framed protobuf payloads and bounded sessions;
- semantic catalog, cache/query/subscription, watch/collection, Health, and
  processor foundations;
- P0 CLI help, version, discovery, blocked dmon, and Health commands;
- systemd unit, default configuration, installed catalogs, public development
  metadata, and release documentation.

The production Metric Catalog remains empty and explicitly
`BLOCKED_EXTERNAL`. PDRL is not linked or packaged. With no production
Provider available, the daemon starts in degraded mode.

Verification includes C/C++ public-header checks, symbol allowlisting,
standalone/embedded contracts, lifecycle and malformed-input tests, staged
install manifest checks, and dynamic dependency scans.

Target qualification is pending the approved PDRL ABI/loader contract,
Discovery and heartbeat mappings, Golden sources, compatibility matrix, and
platform lifecycle procedures. This baseline does not claim AC-3, target
Health, target Golden Source, or full AC-11 PASS.
