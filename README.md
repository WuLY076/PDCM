# PDCM

PDCM is node-level device management and runtime-insight infrastructure for
internally developed TPUs.

The P0 architecture targets FPGA and EMU single-device nodes. The current
development milestone builds the complete PDCM path against a deterministic
MockProvider while the external PDRL contract remains blocked.

## Repository boundaries

- `product/` contains production code and is the only mandatory input to the
  future internal GitLab synchronization.
- `testkit/` contains GitHub-only MockProvider code, mock catalogs, fixtures,
  and integration tests.
- `docs/` contains the RFC and TDD suite.
- `sync/` documents and validates the export policy.

Production code must build independently from the testkit:

```sh
cmake -S product -B build-product -G Ninja -DBUILD_TESTING=ON
cmake --build build-product
ctest --test-dir build-product --output-on-failure
```

The complete development workspace is built with:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Validate the future GitLab boundary with:

```sh
tools/verify-sync-boundary.sh
```
