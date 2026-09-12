#!/usr/bin/env python3
"""Issue #3669: IsolationDeny audit keys the per-Evaluator caller principal —
layout-only denials are no longer mislabeled unset-principal.

Contract (one row per AC):
  AC1  record_audit takes the caller principal; entry.current = caller;
       layout-only (caller set + ref unstamped) gets its own
       isolation-deny:unstamped-ref reason; SE tenant keyed by caller
  AC2  unset-principal string kept for caller == 0 (helper arm)
  AC3  ref-tenant=%llu string kept for foreign stamped refs (helper arm)
  AC4  all check_boundary_ex record_audit call sites pass `cur`
  AC5  Soft/Off: allow rows still emit no IsolationDeny SE (deny-only emit
       unchanged); tests live in the existing isolation family
  AC6  check_workspace_isolation stamps last_mutate_error_ from the same
       isolation_deny_reason source (no dual-track vs resolve_stamped)

Exit 0 = all rows satisfied. --self-test checks the row helpers on
synthetic text, not the live tree.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _rows(must, must_count, must_before) -> list[str]:
    fails: list[str] = []

    iso = _read("src/core/workspace_isolation.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # AC1: caller-keyed audit row + dedicated layout-only reason.
    must("void record_audit(TenantId caller, TenantId target, TenantId ref_tenant, bool denied", "AC1 signature", iso)
    must("entry.current = caller;", "AC1 entry keying", iso)
    must('"isolation-deny:unstamped-ref"', "AC1 unstamped-ref reason", iso)
    must("caller != 0 ? caller : target", "AC1 SE caller tenant", iso)
    must("isolation_deny_reason", "AC1 reason SSOT", iso)

    # AC2/AC3: existing strings preserved as helper arms.
    must('"isolation-deny:unset-principal"', "AC2 unset-principal arm", iso)
    must("isolation-deny:ref-tenant=%llu", "AC3 ref-tenant arm", iso)

    # AC4: every check_boundary_ex record_audit call site passes `cur`.
    must_count("record_audit(cur, target, ref_tenant", "AC4 call sites x4", iso, 4)

    # AC5: deny-only SE emit unchanged (allow returns before the SE).
    must("if (!denied)\n            return;", "AC5 allow no SE", iso)

    # AC6: Agent error stamped from the same reason source.
    must("isolation_deny_reason(capability_tenant_id_, ref_tenant", "AC6 stamp", sec)
    must("last_mutate_error_ =", "AC6 stamp target", sec)
    must_before(
        "AC6 stamp after deny",
        sec.find("if (!ok) {"),
        sec.find("isolation_deny_reason(capability_tenant_id_, ref_tenant"),
    )

    # Tests in the existing isolation family.
    for ac in ("3669 AC1", "3669 AC2", "3669 AC3", "3669 AC4", "3669 AC5", "3669 AC6"):
        must(ac, f"AC5 test {ac}", test)
    must("set_effect_fiber_id_override(3011)", "AC1 fiber probe", test)
    if _read("tests/core/test_issue_3669.cpp") or _read("tests/issues/test_issue_3669.cpp"):
        fails.append("AC5: test_issue_3669.cpp present (forbidden per #81934)")
    if _read("docs/design/3669-isolation-audit-caller.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_isolation_audit_caller_3669", "AC5 build.py", build)
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
            fails.append(f"{label}: expected deny branch before stamp (deny={early}, stamp={late})")

    fails = _rows(must, must_count, must_before)

    if fails:
        print("FAIL #3669 isolation_audit_caller:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3669 isolation_audit_caller: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    good = (
        "void record_audit(TenantId caller, TenantId target, TenantId ref_tenant, bool denied\n"
        "entry.current = caller;\n"
        "isolation-deny:unstamped-ref\n"
        "caller != 0 ? caller : target\n"
        "record_audit(cur, target, ref_tenant\n"
    )
    must("void record_audit(TenantId caller, TenantId target, TenantId ref_tenant, bool denied", "self signature", good)
    must("entry.current = caller;", "self entry", good)
    if "entry.current = current.id;" in good:
        failures.append("self: stripped sample must not carry the old keying")
    print(
        "SELF-TEST OK: check_isolation_audit_caller_3669 row helpers" if not failures else f"SELF-TEST FAIL: {failures}"
    )
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
