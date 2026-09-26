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

## Repository layout

- `product/include/` contains the public ABI, while `product/src/` contains
  internal libraries.
- `product/apps/` contains executable entry points and `product/tests/`
  contains product verification.
- `testkit/tests/` is grouped into unit, component, and end-to-end suites.
- `docs/architecture/` contains the RFC and `docs/tdd/` contains design
  specifications.
- `tools/ci/` contains workspace gates and `tools/sync/` contains export
  checks.

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
tools/sync/verify-sync-boundary.sh
```

Run the complete TDD-10 gate with:

```sh
tools/ci/run-release-gate.sh
```

The command runs the normal workspace tests, ASAN/UBSAN tests, and the
standalone product boundary check. It writes
`build/release-gate/PDCM_P0_RELEASE_GATE.md` with scope-aware AC-1 through
AC-11 and Provider-loader status. A successful command does not override
`BLOCKED_EXTERNAL` target requirements.
