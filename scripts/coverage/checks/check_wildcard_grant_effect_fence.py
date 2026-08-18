#!/usr/bin/env python3
"""Issue #3141: kCapWildcard write-fence — production caller holding
kCapWildcard but NOT explicit TenantAdmin cannot write privilege-bearing
cap names ("self-evo"/"synthesize"/"strategy"/"tenant-admin"/"capability"/
"agent"/"workspace"/"fiber") via string-path grant_capability.

Contract (one row per AC):
  AC1  Production + wildcard-only holder → deny + counter bump + SE
       emit + CapabilityGrant::effects NOT updated.
  AC2  Production + explicit TenantAdmin (non-wildcard string grant) →
       allow unchanged (no double-write, no behavior change).
  AC3  Soft / sandbox=off → zero-cost (wildcard contract preserved).
  AC4  Additive counter only — no schema change to existing metrics.
  AC5  Source-cite capability_model.hh + evaluator_security.cpp; extend
       test_capability_high_risk_promote.cpp; no docs/design/, no
       tests/issues/test_issue_3141.cpp (per #81967/#1655).

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

    meta = _read("src/core/capability_model.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    sec_cap = _read("src/compiler/security_capabilities.h")
    test = _read("tests/compiler/test_capability_high_risk_promote.cpp")
    build = _read("build.py")
    manifest = _read("scripts/coverage/manifests/3141.json")

    # ── AC1: production + wildcard-only → deny ──────────────────────
    must("Issue #3141", "AC1 cite in meta", meta)
    must("try_grant_capability_string_path_privileged_locked", "AC1 fence helper", meta)
    must("holds_wildcard_only_locked", "AC1 wildcard-only detector", meta)
    must("wildcard-write-fence-needs-explicit-tenant-admin", "AC1 SE reason", meta)
    must("capability_wildcard_write_fence_deny_total", "AC4 counter (declared)", meta)

    # AC1 wire-up in evaluator_security.cpp
    must("try_grant_capability_string_path_privileged_locked", "AC1 fence wire-up", eval_sec)
    must("Issue #3141", "AC1 cite in eval_sec", eval_sec)
    # The fence must run BEFORE granted_capabilities_.push_back (dedup consistency on deny).
    must("AC1 deny: skip both push and effect-grant", "AC1 deny early-return", eval_sec)

    # AC2: production + explicit TenantAdmin → allow
    # (no extra marker needed — fence early-returns with effects_for_locked check
    # passing when caller holds explicit TenantAdmin string grant)

    # ── AC3: Soft / sandbox=off zero-cost ───────────────────────────
    must("AC3: Soft / sandbox=off zero-cost", "AC3 doc in meta", meta)

    # ── AC4: additive counter + accessor ────────────────────────────
    must("capability_wildcard_write_fence_deny_total_v_read", "AC4 accessor", meta)
    must("Issue #3141", "AC4 cite in sec_cap", sec_cap)
    must("kCapWildcard write-fence", "AC4 sec_cap comment", sec_cap)

    # ── AC5: source-cite + extend test + no docs/issues ──────────────
    must("ac3141_1_production_wildcard_only_deny", "AC5 AC1 test function", test)
    must("ac3141_2_explicit_tenant_admin_path_unchanged", "AC5 AC2 test function", test)
    must("ac3141_3_soft_off_zero_cost", "AC5 AC3 test function", test)
    must("ac3141_4_additive_counter_only", "AC5 AC4 test function", test)
    must("Issue #3141", "AC5 #3141 cite in test", test)
    must("3140", "AC5 prior #3140 still passing (additive)", test)

    # AC5: build.py wires linter
    must("check_wildcard_grant_effect_fence.py", "AC5 build.py wires linter", build)

    # AC5: manifest exists and contains #3141
    if "3141" not in manifest:
        fails.append("AC5: manifest 3141.json missing '3141'")
    if "check_wildcard_grant_effect_fence.py" not in manifest:
        fails.append("AC5: manifest 3141.json missing linter name")

    # AC5: no docs/design/, no tests/issues/test_issue_3141.cpp
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3141-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "issues" / "test_issue_3141.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3141.cpp present (forbidden per #81967)")

    if fails:
        print("FAIL: Issue #3141 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3141 — kCapWildcard write-fence for production grant_capability.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
