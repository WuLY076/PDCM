# PDCM production source tree

This directory is the self-contained production boundary exported to the
future internal GitLab repository. It must configure, build, test, and install
without the workspace-level `testkit/`.

## Layout

- `include/pdcm/`: installed public C ABI headers.
- `src/`: internal libraries, organized by runtime component.
- `apps/`: the `pdcm-cli` and `pdcm-daemon` executables.
- `proto/local/v1/`: versioned local IPC protocol.
- `catalog/`: production target catalogs and schema documentation.
- `config/`: default runtime configuration and systemd unit.
- `cmake/`: build modules, install rules, package templates, and ABI map.
- `tests/`: unit, integration, contract, and install verification.
- `docs/release/`: installed release documentation.
- `verification/`: versioned acceptance and Provider-loader status.
- `tools/release/`: production-tree release contract checks.

Each internal source component owns its CMake target definition. The top-level
`CMakeLists.txt` only establishes project policy, orders components, installs
artifacts, and enables tests.

## Build

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
