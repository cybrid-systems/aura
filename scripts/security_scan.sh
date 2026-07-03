#!/usr/bin/env bash
# Issue #675: dependency / filesystem vulnerability scan.
# Uses Trivy when available; falls back to pip-audit for Python deps.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "═══ Security scan (Issue #675) ═══"

if command -v trivy >/dev/null 2>&1; then
  echo "→ Trivy filesystem scan"
  trivy fs --scanners vuln,misconfig,secret \
    --severity HIGH,CRITICAL \
    --exit-code 1 \
    --ignore-unfixed \
    --skip-dirs build,build_asan,build_ubsan,build_tsan,build_repro_a,build_repro_b,build_dbg,build_ubsan \
    "$ROOT"
  echo "✓ Trivy scan clean (HIGH/CRITICAL)"
  exit 0
fi

if command -v pip-audit >/dev/null 2>&1; then
  echo "→ pip-audit (Trivy not installed)"
  pip-audit -r requirements-dev.txt --strict
  echo "✓ pip-audit clean"
  exit 0
fi

echo "::warning::Neither trivy nor pip-audit found — running requirements sanity check only"
if [[ ! -f requirements-dev.txt ]]; then
  echo "::error::requirements-dev.txt missing"
  exit 1
fi
if [[ ! -f .github/dependabot.yml ]]; then
  echo "::error::.github/dependabot.yml missing (Dependabot not configured)"
  exit 1
fi
echo "✓ Dependabot config present; install trivy or pip-audit for full scanning"
exit 0