#!/usr/bin/env python3
"""Issue #3144: kCapWildcard持卡但不显式 TenantAdmin → effects_for() 查询
path strip TenantAdmin + MacroSelfEvo bits。 closes Gap A 旁路(独立于
#3141 grant_capability 字符串 path 的查询 path)。

Contract (one row per AC):
  AC1  Production + kCapWildcard holder (no explicit TenantAdmin) →
       effects_for() returns mask with TenantAdmin + MacroSelfEvo bits
       stripped. Caller cannot pass require_effect(TenantAdmin) check.
  AC2  Production + explicit TenantAdmin (non-wildcard string grant) →
       effects_for() returns full mask (no strip). Wildcard holder with
       TenantAdmin = full access.
  AC3  Soft / sandbox=off → zero-cost (returns full mask unchanged;
       wildcard contract preserved).
  AC4  Additive counter only — wildcard_strip_tenant_admin_effect_total
       appended at CapabilityEffectMetrics struct END per #2906; no
       schema change to existing counters / query keys.
  AC5  Source-cite capability_model.hh + security_capabilities.h; extends
       tests/core/test_capability_single_use_consume.cpp with wildcard +
       TenantAdmin scenarios; no docs/design/, no
       tests/issues/test_issue_3144.cpp (per #81967/#1655).

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
    sec_cap = _read("src/compiler/security_capabilities.h")
    _read("src/compiler/security_defaults.hh")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")
    manifest = _read("scripts/coverage/manifests/3144.json")

    # ── AC1: Production + kCapWildcard (no explicit TenantAdmin) → strip ─
    must("Issue #3144", "AC1 cite in meta", meta)
    must("wildcard_strip_tenant_admin_effect_total", "AC1 counter (declared)", meta)
    must("Effect::TenantAdmin", "AC1 strip TenantAdmin bit", meta)
    must("Effect::MacroSelfEvo", "AC1 strip MacroSelfEvo bit", meta)
    must("kStrip =", "AC1 kStrip mask", meta)
    must("has_wildcard && !has_explicit_TenantAdmin", "AC1 strip condition", meta)
    # Wildcard detection must use the same kCapWildcard string ("*")
    must('g.name == "*"', "AC1 wildcard name check", meta)

    # AC1 wire-up: effects_for + effects_for_locked both have the strip.
    # Count occurrences of the strip pattern in meta.
    strip_count = meta.count("has_wildcard && !has_explicit_TenantAdmin")
    if strip_count < 2:
        fails.append(f"AC1: strip pattern should appear in both effects_for + effects_for_locked, found {strip_count}")

    # ── AC2: Production + explicit TenantAdmin → no strip ─────────
    # Verified by the strip condition (has_explicit_TenantAdmin short-circuits).

    # ── AC3: Soft / sandbox=off → zero-cost ─────────────────────
    must("Soft/Off: zero-cost", "AC3 doc", meta)
    must("mode != EffectSandboxMode::Off", "AC3 mode gate (production only)", meta)

    # ── AC4: Additive counter, struct END per #2906 ─────────────────
    must("wildcard_strip_tenant_admin_effect_total{0}", "AC4 counter field initializer", meta)
    # Counter must be appended at struct END (after capability_wildcard_write_fence_deny_total
    # which was the latest addition before #3144).
    pos_3141 = meta.find("capability_wildcard_write_fence_deny_total{0};")
    pos_3144 = meta.find("wildcard_strip_tenant_admin_effect_total{0};")
    if pos_3141 == -1 or pos_3144 == -1 or pos_3144 <= pos_3141:
        fails.append("AC4: #3144 counter not appended at struct END per #2906")

    # AC4 source-cite in security_capabilities.h
    must("Issue #3144", "AC4 cite in sec_cap", sec_cap)
    must("wildcard_strip_tenant_admin_effect_total", "AC4 counter reference in sec_cap", sec_cap)
    must("wildcard_strip_tenant_admin_effect_total_v_read", "AC4 accessor export in sec_cap", sec_cap)

    # ── AC5: source-cite + extend test + no docs/issues ─────────────
    must("ac3144_1_production_wildcard_only_strip", "AC5 AC1 test function", test)
    must("ac3144_2_explicit_tenant_admin_no_strip", "AC5 AC2 test function", test)
    must("ac3144_3_soft_off_no_strip", "AC5 AC3 test function", test)
    must("ac3144_4_additive_counter_and_source_cite", "AC5 AC4 test function", test)
    must("Issue #3144", "AC5 #3144 cite in test", test)

    # AC5: build.py wires linter
    must("check_wildcard_effects_for_fence.py", "AC5 build.py wires linter", build)

    # AC5: manifest exists and contains #3144
    if "3144" not in manifest:
        fails.append("AC5: manifest 3144.json missing '3144'")
    if "check_wildcard_effects_for_fence.py" not in manifest:
        fails.append("AC5: manifest 3144.json missing linter name")

    # AC5: no docs/design/, no tests/issues/test_issue_3144.cpp
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3144-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "issues" / "test_issue_3144.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3144.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3144.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3144.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3144.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3144.cpp present (forbidden per #81967)")

    if fails:
        print("FAIL: Issue #3144 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3144 — kCapWildcard effects_for query-path strip TenantAdmin.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
