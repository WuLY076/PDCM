#!/usr/bin/env bash
set -euo pipefail

product_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release_manifest="${product_root}/verification/p0-release-status.tsv"
loader_manifest="${product_root}/verification/provider-loader-status.tsv"
report_path=""

while (($# > 0)); do
  case "$1" in
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

fail() {
  echo "release contract: $*" >&2
  exit 1
}

[[ -f "${release_manifest}" ]] || fail "release status manifest is missing"
[[ -f "${loader_manifest}" ]] || fail "loader status manifest is missing"

declare -A statuses scopes evidence seen
while IFS=$'\t' read -r ac status scope refs; do
  [[ "${ac}" == "ac" ]] && continue
  [[ -n "${ac}" && -n "${status}" && -n "${scope}" && -n "${refs}" ]] ||
    fail "manifest contains an incomplete row"
  [[ "${ac}" =~ ^AC-([1-9]|10|11)$ ]] || fail "unknown acceptance item ${ac}"
  [[ -z "${seen[${ac}]:-}" ]] || fail "duplicate acceptance item ${ac}"
  case "${status}" in
  PASS | FAIL | BLOCKED_EXTERNAL | NOT_SUPPORTED_BY_SCOPE | NOT_APPLICABLE_CONDITIONAL | WAIVED) ;;
  *) fail "invalid release status ${status} for ${ac}" ;;
  esac
  [[ "${status}" != "WAIVED" ]] || fail "baseline must not contain waivers"
  statuses["${ac}"]="${status}"
  scopes["${ac}"]="${scope}"
  evidence["${ac}"]="${refs}"
  seen["${ac}"]=1
done <"${release_manifest}"

for number in {1..11}; do
  [[ -n "${seen[AC-${number}]:-}" ]] || fail "AC-${number} is missing"
done

for ac in AC-5 AC-6 AC-10; do
  [[ "${statuses[${ac}]}" == "NOT_SUPPORTED_BY_SCOPE" ]] ||
    fail "${ac} must remain NOT_SUPPORTED_BY_SCOPE"
done
for ac in AC-2 AC-3 AC-4 AC-7 AC-9 AC-11; do
  [[ "${statuses[${ac}]}" == "BLOCKED_EXTERNAL" ]] ||
    fail "${ac} must remain BLOCKED_EXTERNAL until approved target inputs exist"
done
for ac in AC-1 AC-8; do
  [[ "${statuses[${ac}]}" == "PASS" ]] ||
    fail "${ac} generic contract gate must pass"
done

declare -A loader_status loader_evidence loader_seen
while IFS=$'\t' read -r loader_case status refs; do
  [[ "${loader_case}" == "case" ]] && continue
  [[ "${loader_case}" =~ ^LDR-00[1-9]$ ]] ||
    fail "unknown loader case ${loader_case}"
  [[ -z "${loader_seen[${loader_case}]:-}" ]] ||
    fail "duplicate loader case ${loader_case}"
  loader_status["${loader_case}"]="${status}"
  loader_evidence["${loader_case}"]="${refs}"
  loader_seen["${loader_case}"]=1
done <"${loader_manifest}"

for number in {1..9}; do
  loader_case="LDR-00${number}"
  [[ -n "${loader_seen[${loader_case}]:-}" ]] ||
    fail "${loader_case} is missing"
  expected="BLOCKED_EXTERNAL"
  if ((number >= 8)); then
    expected="PASS"
  fi
  [[ "${loader_status[${loader_case}]}" == "${expected}" ]] ||
    fail "${loader_case} must be ${expected}"
done

required_docs=(
  INSTALL.md CLI_REFERENCE.md SINGLE_CARD_SUPPORT_MATRIX.md
  P0_METRIC_CATALOG.md FIRMWARE_HEARTBEAT_HEALTH_CONTRACT.md
  PUBLIC_API_REFERENCE.md ERROR_CODES.md TROUBLESHOOTING.md
  COMPATIBILITY_MATRIX.md KNOWN_ISSUES.md RELEASE_NOTES.md
)
for document in "${required_docs[@]}"; do
  [[ -s "${product_root}/docs/release/${document}" ]] ||
    fail "required release document is missing or empty: ${document}"
done

for target in fpga emu; do
  catalog="${product_root}/catalog/targets/${target}.yaml"
  [[ -f "${catalog}" ]] || fail "${target} catalog is missing"
  grep -Eq '^metrics_status: BLOCKED_EXTERNAL$' "${catalog}" ||
    fail "${target} catalog must be BLOCKED_EXTERNAL"
  grep -Eq '^metrics: \[\]$' "${catalog}" ||
    fail "${target} catalog must not declare production metrics"
  grep -Eq 'native_mapping: TBD_TDD08' "${catalog}" ||
    fail "${target} heartbeat mapping must remain unresolved"
done

grep -Eq 'Status: `BLOCKED_EXTERNAL`' "${product_root}/docs/release/P0_METRIC_CATALOG.md" ||
  fail "Metric Catalog documentation does not expose its blocked status"
grep -Eq 'Matrix version: `UNFROZEN-TDD08`' "${product_root}/docs/release/COMPATIBILITY_MATRIX.md" ||
  fail "Compatibility Matrix must remain explicitly unfrozen"
if grep -ERq 'pdcm-cli[[:space:]]+(diag|reset|fault|inject)' "${product_root}/docs/release"; then
  fail "release documentation advertises a forbidden P0 command"
fi
if grep -ERq 'pdcm_(operation|diagnostic|fault|inject)' "${product_root}/include/pdcm"; then
  fail "public headers expose an out-of-scope P0 symbol"
fi

overall="PASS"
for number in {1..11}; do
  status="${statuses[AC-${number}]}"
  if [[ "${status}" == "FAIL" ]]; then
    overall="FAIL"
    break
  fi
  if [[ "${status}" == "BLOCKED_EXTERNAL" ]]; then
    overall="BLOCKED_EXTERNAL"
  fi
done

if [[ -n "${report_path}" ]]; then
  mkdir -p "$(dirname "${report_path}")"
  revision="unknown"
  if git -C "${product_root}" rev-parse --verify HEAD >/dev/null 2>&1; then
    revision="$(git -C "${product_root}" rev-parse HEAD)"
  fi
  {
    echo "# PDCM P0 release gate report"
    echo
    echo "- Overall status: \`${overall}\`"
    echo "- PDCM version: \`0.1.0\`"
    echo "- Source revision: \`${revision}\`"
    echo "- Workspace tests: \`${PDCM_GATE_WORKSPACE_TESTS:-not supplied}\`"
    echo "- Sanitizer tests: \`${PDCM_GATE_SANITIZER_TESTS:-not supplied}\`"
    echo "- Product boundary: \`${PDCM_GATE_PRODUCT_BOUNDARY:-not supplied}\`"
    echo
    echo "| AC | Status | Scope | Evidence |"
    echo "|---|---|---|---|"
    for number in {1..11}; do
      ac="AC-${number}"
      echo "| ${ac} | ${statuses[${ac}]} | ${scopes[${ac}]} | ${evidence[${ac}]} |"
    done
    echo
    echo "## Provider loader matrix"
    echo
    echo "| Case | Status | Evidence |"
    echo "|---|---|---|"
    for number in {1..9}; do
      loader_case="LDR-00${number}"
      echo "| ${loader_case} | ${loader_status[${loader_case}]} | ${loader_evidence[${loader_case}]} |"
    done
    echo
    echo "## Versioned artifact hashes"
    echo
    echo '```text'
    artifacts=("catalog/targets/fpga.yaml"
               "catalog/targets/emu.yaml"
               "docs/release/P0_METRIC_CATALOG.md"
               "docs/release/FIRMWARE_HEARTBEAT_HEALTH_CONTRACT.md"
               "docs/release/COMPATIBILITY_MATRIX.md"
               "verification/p0-release-status.tsv"
               "verification/provider-loader-status.tsv")
    (cd "${product_root}" && sha256sum "${artifacts[@]}")
    echo '```'
    echo
    echo "Target release remains blocked until TDD-08, the production Metric"
    echo "Catalog, native heartbeat mapping, Golden sources, compatibility"
    echo "combinations, and platform lifecycle procedures are approved."
  } >"${report_path}"
fi

echo "release contract is internally consistent; overall=${overall}"
