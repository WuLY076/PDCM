#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_root="${repo_root}/build/release-gate"
report_path=""

while (($# > 0)); do
  case "$1" in
  --build-root)
    if (($# < 2)); then
      echo "--build-root requires a path" >&2
      exit 2
    fi
    build_root="$2"
    shift 2
    ;;
  --report)
    if (($# < 2)); then
      echo "--report requires a path" >&2
      exit 2
    fi
    report_path="$2"
    shift 2
    ;;
  *)
    echo "unknown argument: $1" >&2
    exit 2
    ;;
  esac
done

if [[ -z "${report_path}" ]]; then
  report_path="${build_root}/PDCM_P0_RELEASE_GATE.md"
fi

mkdir -p "${build_root}"

cmake -S "${repo_root}" -B "${build_root}/workspace" -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "${build_root}/workspace"
ctest --test-dir "${build_root}/workspace" --output-on-failure
workspace_count="$(ctest --test-dir "${build_root}/workspace" -N |
  awk '/Total Tests:/ {print $3}')"
workspace_tests="${workspace_count}/${workspace_count} passed"

cmake -S "${repo_root}" -B "${build_root}/sanitizers" -G Ninja -DBUILD_TESTING=ON -DPDCM_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "${build_root}/sanitizers"
ctest --test-dir "${build_root}/sanitizers" --output-on-failure
sanitizer_count="$(ctest --test-dir "${build_root}/sanitizers" -N |
  awk '/Total Tests:/ {print $3}')"
sanitizer_tests="${sanitizer_count}/${sanitizer_count} passed (ASAN/UBSAN)"

"${repo_root}/tools/sync/verify-sync-boundary.sh"

env PDCM_GATE_WORKSPACE_TESTS="${workspace_tests}" PDCM_GATE_SANITIZER_TESTS="${sanitizer_tests}" PDCM_GATE_PRODUCT_BOUNDARY="PASS" "${repo_root}/product/tools/release/verify-release-contract.sh" --report "${report_path}"

echo "release gate report: ${report_path}"
