# Troubleshooting

Start with:

```sh
systemctl status pdcm.service
journalctl -u pdcm.service
pdcm-cli version --format json --verbose
```

## Daemon unavailable

Confirm that `/run/pdcm/pdcm.sock` exists, is owned by the service account and
`pdcm` group, and is not world accessible. Validate configuration with:

```sh
/usr/sbin/pdcm-daemon --config /etc/pdcm/pdcm.conf --check-config
```

## Daemon is degraded

Missing, unloadable, incompatible, or initialization-failing PDRL must leave
the daemon running in `DEGRADED`; it must not crash-loop. Version output
identifies the stable failure phase. PDRL is separately delivered, and users
cannot override its path through CLI, API, or configuration.

## Dmon is blocked

`CATALOG_BLOCKED_EXTERNAL` is expected until the production Metric Catalog is
approved. Do not substitute test names or values.

## Health is unknown

Read the stable code and limitation. Missing, stale, timed-out, failed, or
unmapped heartbeat evidence correctly yields UNKNOWN rather than hardware
ERROR. Provider unavailable maps to CLI exit 5 when no current catalog/result
can be served.

Release scans verify that no PDCM runtime binary has a `libpdrl` DT_NEEDED
entry.
