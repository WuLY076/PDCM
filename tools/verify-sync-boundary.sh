#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$(mktemp -d)"
trap 'rm -rf "${build_root}"' EXIT

if rg -n '#[[:space:]]*include[[:space:]]*[<"]testkit/|add_subdirectory\([^)]*testkit|pdcm_mock_provider' \
    "${repo_root}/product"; then
  echo "product/ contains a forbidden testkit dependency" >&2
  exit 1
fi

cmake -S "${repo_root}/product" -B "${build_root}/product" -G Ninja \
  -DBUILD_TESTING=ON
cmake --build "${build_root}/product"
ctest --test-dir "${build_root}/product" --output-on-failure

if readelf -d "${build_root}/product/libpdcm.so" | rg -i 'libpdrl'; then
  echo "libpdcm.so has a forbidden libpdrl.so dynamic dependency" >&2
  exit 1
fi

echo "product/ is independent from testkit/ and libpdrl.so"
