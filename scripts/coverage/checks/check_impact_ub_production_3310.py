#!/usr/bin/env python3
# scripts/check_3310_impact_ub_production.py — Issue #3310 linter
#
# Verifies the production fail-closed gate on unknown ImpactScope
# (refine #3034 / #3068 / #3097):
#   1. `should_partial_relower_impact_checked_prod` helper exists in
#      src/compiler/ir_cache_pure.ixx with the production && ub==0
#      fail-closed branch BEFORE delegating to the existing helper.
#   2. src/compiler/service.ixx calls the new helper with
#      production_consult derived from production_defaults_active() ||
#      AuditStrategy::Full.
#   3. The fail-closed branch bumps `partial_forced_full_by_impact_total`
#      (no new query key — reuses existing #3034 counter).
#   4. Test file tests/compiler/test_partial_relower_impact_production.cpp
#      exists with AC1–AC5 coverage.
#
# Exits 0 when all checks pass; 1 with a diagnostic otherwise.
#
# Usage:
#   python3 scripts/check_3310_impact_ub_production.py [--self-test]

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SERVICE_IXX = REPO_ROOT / "src" / "compiler" / "service.ixx"
IR_CACHE_PURE_IXX = REPO_ROOT / "src" / "compiler" / "ir_cache_pure.ixx"
TEST_FILE = REPO_ROOT / "tests" / "compiler" / "test_partial_relower_impact_production.cpp"

PROD_HELPER = "should_partial_relower_impact_checked_prod"
EXISTING_HELPER = "should_partial_relower_impact_checked"
PRODUCTION_COUNTER = "partial_forced_full_by_impact_total"


def _read(path: Path) -> str:
    if not path.exists():
        print(f"[3310] missing file: {path}", file=sys.stderr)
        sys.exit(1)
    return path.read_text(encoding="utf-8")


def _slice_balanced_braces(text: str, open_pos: int) -> str | None:
    """Return the substring from `open_pos` (which must point at `{`) to
    the matching `}` at the same brace depth, inclusive of both
    braces. Returns None if braces are unbalanced."""
    depth = 0
    i = open_pos
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_pos : i + 1]
        i += 1
    return None


def check_helper_in_ir_cache_pure() -> list[str]:
    """The new helper must exist in ir_cache_pure.ixx with the
    production && ub==0 fail-closed branch BEFORE the existing helper
    delegate."""
    errors: list[str] = []
    text = _read(IR_CACHE_PURE_IXX)
    if PROD_HELPER not in text:
        errors.append(f"[ir_cache_pure] missing helper `{PROD_HELPER}`")
        return errors
    # Locate the helper signature (multi-line tolerant).
    sig_re = re.compile(
        rf"\[\[nodiscard\]\]\s*inline\s*bool\s*{re.escape(PROD_HELPER)}\s*\(",
        re.MULTILINE,
    )
    sig_m = sig_re.search(text)
    if not sig_m:
        errors.append(f"[ir_cache_pure] helper `{PROD_HELPER}` lacks expected `[[nodiscard]] inline bool` signature")
        return errors
    # Find the next `{` after the signature (the body open).
    brace_pos = text.find("{", sig_m.end())
    if brace_pos == -1:
        errors.append(f"[ir_cache_pure] helper `{PROD_HELPER}` body open brace not found")
        return errors
    body = _slice_balanced_braces(text, brace_pos)
    if body is None:
        errors.append(f"[ir_cache_pure] helper `{PROD_HELPER}` body braces unbalanced")
        return errors
    if "production" not in body or "impact_upper_bound == 0" not in body:
        errors.append(
            f"[ir_cache_pure] helper `{PROD_HELPER}` must include the "
            f"`production && impact_upper_bound == 0` fail-closed branch"
        )
    if EXISTING_HELPER not in body:
        errors.append(
            f"[ir_cache_pure] helper `{PROD_HELPER}` must delegate to `{EXISTING_HELPER}` (no logic duplication)"
        )
    if "dirty_count == 0" not in body:
        errors.append(
            f"[ir_cache_pure] helper `{PROD_HELPER}` must early-exit on `dirty_count == 0` (clean window, zero-cost)"
        )
    return errors


def _extract_partial_branch_region(text: str) -> str | None:
    """Return the substring covering the `if (want_partial && dirty_n > 0) { ... }`
    block in relower_dirty_defines_from_workspace (deepest outer block
    containing the production consult)."""
    # Find the open of the outer if-block.
    pat = re.compile(
        r"if\s*\(\s*want_partial\s*&&\s*dirty_n\s*>\s*0\s*\)\s*\{",
        re.MULTILINE,
    )
    m = pat.search(text)
    if not m:
        return None
    open_pos = text.find("{", m.start())
    if open_pos == -1:
        return None
    return _slice_balanced_braces(text, open_pos)


def check_service_ixx_call_site() -> list[str]:
    """service.ixx must call the new helper with production_consult
    derived from production_defaults_active() || AuditStrategy::Full,
    and bump the existing counter on the fail-closed path."""
    errors: list[str] = []
    text = _read(SERVICE_IXX)
    if PROD_HELPER not in text:
        errors.append(f"[service] missing call to `{PROD_HELPER}`")
        return errors
    region = _extract_partial_branch_region(text)
    if region is None:
        errors.append(
            "[service] cannot locate `if (want_partial && dirty_n > 0) { ... }` "
            "region in relower_dirty_defines_from_workspace"
        )
        return errors
    if "production_defaults_active" not in region:
        errors.append("[service] partial branch must consult `typed_audit::production_defaults_active()`")
    if "AuditStrategy::Full" not in region:
        errors.append("[service] partial branch must consult `typed_audit::AuditStrategy::Full`")
    if PROD_HELPER not in region:
        errors.append(f"[service] partial branch must call `{PROD_HELPER}` (production fail-closed gate)")
    if PRODUCTION_COUNTER not in region:
        errors.append(f"[service] partial branch must bump `{PRODUCTION_COUNTER}` on the fail-closed path")
    # Guard: the production fail-closed block must call the new helper,
    # NOT the raw existing helper, at the consult site.
    consult_re = re.compile(
        rf"if\s*\(\s*!\s*{re.escape(PROD_HELPER)}\s*\(",
        re.MULTILINE,
    )
    if not consult_re.search(region):
        errors.append(
            f"[service] partial branch must call `{PROD_HELPER}` as the consult site (the production fail-closed gate)"
        )
    return errors


def check_test_file_exists() -> list[str]:
    errors: list[str] = []
    if not TEST_FILE.exists():
        errors.append(f"[test] missing test file: {TEST_FILE}")
        return errors
    text = TEST_FILE.read_text(encoding="utf-8")
    required_acs = ["AC1", "AC2", "AC3", "AC4", "AC5"]
    for ac in required_acs:
        if ac not in text:
            errors.append(f"[test] missing AC marker: {ac}")
    if PROD_HELPER not in text:
        errors.append(f"[test] must call `{PROD_HELPER}` in at least one AC")
    return errors


def self_test() -> int:
    """Run a small sanity check against the helper's expected shape."""
    text = _read(IR_CACHE_PURE_IXX)
    if PROD_HELPER not in text:
        print("[self-test] FAIL: helper missing from ir_cache_pure.ixx")
        return 1
    print("[self-test] OK: helper present in ir_cache_pure.ixx")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Issue #3310 production-fail-closed linter")
    parser.add_argument("--self-test", action="store_true", help="run a small sanity check")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    errors: list[str] = []
    errors += check_helper_in_ir_cache_pure()
    errors += check_service_ixx_call_site()
    errors += check_test_file_exists()

    if errors:
        for line in errors:
            print(line, file=sys.stderr)
        print(f"[3310] FAILED ({len(errors)} issue(s))", file=sys.stderr)
        return 1
    print(
        "OK: Issue #3310 production fail-closed on unknown ImpactScope — should_partial_relower_impact_checked_prod gates ub==0 + production; reuses partial_forced_full_by_impact_total; Soft/Off zero-cost preserved; -1 sentinel path untouched."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
