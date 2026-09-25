# Installation, upgrade, and removal

This guide applies to PDCM 0.1.0 on a single-device FPGA or EMU node.

## Install

The platform package must create the locked service account and group
`pdcm:pdcm`, install the staged files, reload systemd, and apply the platform
policy for enabling or starting the service. PDRL is a separate package and is
not copied or linked by the PDCM package.

A source staging install can be produced without root:

```sh
cmake -S product -B build-product -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-product
DESTDIR="$PWD/stage" cmake --install build-product
```

The production package owns `/usr/lib*/libpdcm.so.0`,
`/usr/bin/pdcm-cli`, `/usr/sbin/pdcm-daemon`, public headers, the systemd
unit, `/etc/pdcm/pdcm.conf`, catalogs, and these documents. It does not own
`libpdrl.so`.

After package installation:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now pdcm.service
pdcm-cli version
pdcm-cli discovery
pdcm-cli health
```

If PDRL is absent, installation still succeeds and the daemon remains running
in `DEGRADED` state. Discovery and health then return unavailable.

## Upgrade

Stop the daemon, atomically replace package-owned files, preserve the existing
configuration using the platform's conffile mechanism, reload systemd, start
the daemon, then run the three smoke commands above. P0 does not promise a
zero-downtime upgrade. An incompatible or unknown PDRL combination must never
be reported as ready.

## Remove

Stop and disable the service, wait for bounded shutdown, remove package-owned
files, reload systemd, and verify that no `pdcm-daemon` process or
`/run/pdcm/pdcm.sock` remains. Configuration and logs are retained or removed
only according to platform package policy.
