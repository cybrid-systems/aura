#!/usr/bin/env python3
"""Issue #3800: grant_cross_tenant TA fence is caller-only.

Under Restricted/Strict, try_grant_cross_tenant_privileged allows only when
the caller holds TenantAdmin. Target-only TA must deny + emit the existing
SE reason; cross_grants table unchanged. Soft/Off zero-cost mint out of scope.

Contract (one row per AC):
  AC1  caller lacks TA, target has TA → deny; cross_grant_bits unchanged
  AC2  caller has TA → allow (mint_principal = caller)
  AC3  Soft/Off zero-cost unchanged
  AC4  #3086 AC3 oracle flipped; cite-first linter; no new query key
  AC5  dual-Evaluator — A (no TA) cannot mint into B (TA) as target

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    iso = _read("src/core/workspace_isolation.hh")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")

    # ── AC1/AC2 fence: caller-only TA; no OR of target ──
    must("Issue #3800", "AC1/AC2 cite", iso)
    must("caller-only TenantAdmin", "AC1/AC2 caller-only marker", iso)
    must("try_grant_cross_tenant_privileged", "AC1/AC2 helper", iso)
    must("const bool caller_ta = has_effect(caller_eff, Effect::TenantAdmin);",
         "AC1/AC2 caller_ta", iso)
    if "caller_ta || target_ta" in iso:
        fails.append("AC1: caller_ta || target_ta still present (must be caller-only)")
    if "target_ta" in iso:
        fails.append("AC1: target_ta residual still present in workspace_isolation.hh")
    # Deny path keeps stable SE reason (#2968).
    must("cross-tenant-grant-needs-tenant-admin", "AC1 SE reason", iso)
    must("cross_tenant_grant_deny_total", "AC1 deny counter", iso)

    # mint_principal on allow is always caller (#3797/#3800).
    mint_win = iso.find("Issue #3800: caller-only TenantAdmin")
    if mint_win < 0:
        fails.append("AC2: #3800 caller-only cite missing near fence")
    else:
        snip = iso[mint_win : mint_win + 600]
        if "*out_mint_principal = caller;" not in snip:
            fails.append("AC2: allow path must bind mint_principal to caller")
        if "caller_ta ? caller : to" in snip:
            fails.append("AC2: legacy caller_ta ? caller : to mint still present")

    # ── AC3 Soft/Off ──
    must("EffectSandboxMode::Off", "AC3 Soft/Off short-circuit", iso)
    must("Soft/Off: no TA fence → mint_principal stays 0", "AC3 Soft mint", iso)

    # ── AC4: #3086 AC3 oracle flipped; linter wired; no invent / query key ──
    must("#3086 AC3/#3800", "AC4 oracle cite", test_iso)
    must("target-only TA → deny", "AC4 oracle deny", test_iso)
    must("cross_grant_bits unchanged when only target holds TenantAdmin",
         "AC4 bits unchanged", test_iso)
    must("check_cross_tenant_grant_caller_ta_3800", "AC4 build.py", build)
    must("Issue #3800", "AC4 build cite", build)
    if "schema-3800" in prim:
        fails.append("AC4: schema-3800 leaked into posture — forbidden")
    if "issue-3800" in prim:
        fails.append("AC4: issue-3800 leaked into posture — forbidden")
    for rel in ("tests/core/test_issue_3800.cpp", "tests/compiler/test_issue_3800.cpp"):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3800-*.md"):
            fails.append(f"AC4: {f.relative_to(ROOT)} exists — forbidden per #1655")

    # ── AC5 dual-Evaluator ──
    must("#3800 AC5", "AC5 dual-eval cite", test_iso)
    must("cannot mint into B", "AC5 dual-eval title", test_iso)
    must("A (no TA) mint into B (TA target) → deny", "AC5 deny CHECK", test_iso)

    if fails:
        print(f"Issue #3800 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3800 cross-tenant grant caller-only TA — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
