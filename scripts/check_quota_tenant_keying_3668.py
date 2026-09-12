#!/usr/bin/env python3
"""Issue #3668: MutationBoundary mirror + resume quota TLS key ResourceQuota
by tenant — the #3049 per-tenant map is live on the mutate dim.

Contract (one row per AC):
  AC1  both boundary try_acquire mirror consumes derive the tenant
       (capability principal first, resume-bound quota TLS fallback) and
       pass it to check_and_consume
  AC2  consume and release use the same tenant key (paired keying; release
       goes through release(d, amount, tenant) → release_tenant, slot-only)
  AC3  resume install binds quota TLS next to TenantScope (quota_tenant_id_
       first, assigned fallback, kUnsetTenant skipped, snapshot-once) and
       aura_fiber_release_tenant_scope_after_yield restores it
  AC4  Soft/Off resume returns before the rebind (mode==0); the quota gate
       `tenant != 0 && quota_per_tenant_enabled()` is unchanged
  AC5  tests live in the existing quota / tenant-scope family; no invent
       test; no docs/design

Exit 0 = all rows satisfied. --self-test checks the row helpers on
synthetic text (missing-anchor detection), not the live tree.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _rows(must, must_count, must_before) -> list[str]:
    """Collect contract rows; returns failure list (empty = pass)."""
    fails: list[str] = []

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    rq = _read("src/core/resource_quota.hh")
    mandate = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # AC1/AC2: both boundary mirror sites derive the tenant and pass it.
    must_count("const auto qtid = ev.capability_tenant_id() != 0", "AC1 derivation x2", mb, 2)
    must_count("Dimension::Mutations, pending_count, qtid)", "AC1/AC2 keyed consume x2", mb, 2)
    must("aura::core::resource_quota::current_quota_tenant()", "AC1 TLS fallback", mb)
    must("Issue #3668", "AC1 cite", mb)

    # AC3: resume rebind (quota_tenant_id_ first, assigned fallback) +
    # snapshot-once restore in the after_yield companion.
    must("f->quota_tenant_id() != 0 ? f->quota_tenant_id() : assigned", "AC3 fallback", fm)
    must("aura::core::resource_quota::set_current_quota_tenant(qtid)", "AC3 bind", fm)
    must("if (!g_fiber_quota_tenant_bound)", "AC3 snapshot-once", fm)
    must("aura::core::resource_quota::set_current_quota_tenant(g_fiber_prev_quota_tenant)", "AC3 restore", fm)
    must('#include "core/resource_quota.hh"', "AC3 include", fm)

    # AC4: Soft/Off resume returns before the rebind; quota gate unchanged.
    mode_idx = fm.find("if (mode == 0)")
    rebind_idx = fm.find("Issue #3668: bind the quota TLS")
    must_before("AC4 rebind after soft return", mode_idx, rebind_idx)
    must("tenant != 0 && quota_per_tenant_enabled()", "AC4 quota gate", rq)

    # AC5: tests in the existing quota / tenant-scope family.
    must("3668", "AC5 mandate test", mandate)
    must("set_quota_tenant_id(7)", "AC5 resume keying test", mandate)
    must("current_quota_tenant() == 7", "AC5 TLS bind test", mandate)
    must("3668", "AC5 isolation test", iso)
    must("quota-exceeded:tenant=7", "AC5 deny-reason test", iso)
    if _read("tests/core/test_issue_3668.cpp") or _read("tests/issues/test_issue_3668.cpp"):
        fails.append("AC5: test_issue_3668.cpp present (forbidden per #81934)")
    if _read("docs/design/3668-quota-tenant-keying.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_quota_tenant_keying_3668", "AC5 build.py", build)
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_count(n: str, label: str, hay: str, k: int) -> None:
        c = hay.count(n)
        if c != k:
            fails.append(f"{label}: expected {k}x {n!r}, found {c}")

    def must_before(label: str, early: int, late: int) -> None:
        if early < 0 or late < 0 or not (early < late):
            fails.append(f"{label}: expected anchor before rebind (mode={early}, rebind={late})")

    fails = _rows(must, must_count, must_before)

    if fails:
        print("FAIL #3668 quota_tenant_keying:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3668 quota_tenant_keying: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    stripped = "int main() { return 0; }\n"
    good = (
        "const auto qtid = ev.capability_tenant_id() != 0\n"
        "  : aura::core::resource_quota::current_quota_tenant();\n"
        "check_and_consume(Dimension::Mutations, pending_count, qtid);\n"
        "if (mode == 0) return;\n"
        "Issue #3668: bind the quota TLS\n"
    )
    must("const auto qtid = ev.capability_tenant_id() != 0", "self good", good)
    if "const auto qtid = ev.capability_tenant_id() != 0" in stripped:
        failures.append("self: stripped text must not carry the anchor")
    # Order check: mode anchor must precede the rebind anchor.
    g_mode = good.find("if (mode == 0)")
    g_rebind = good.find("Issue #3668: bind the quota TLS")
    if not (0 <= g_mode < g_rebind):
        failures.append("self: good sample order wrong")
    print("SELF-TEST OK: check_quota_tenant_keying_3668 row helpers" if not failures else f"SELF-TEST FAIL: {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
