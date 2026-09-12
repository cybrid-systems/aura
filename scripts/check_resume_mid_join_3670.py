#!/usr/bin/env python3
"""Issue #3670: steal×resume fiber-principal-mismatch IsolationDeny joins the
SSOT mid — the #3594 phantom mid=1 coercion is gone from the resume path.

Contract (one row per AC):
  AC1  production resume mismatch: SE mid == Mutation epoch (0 stays 0;
       no phantom 1 to false-join mid=1 grant/session rows)
  AC2  the mid joins the live TypedMid/session mid via the SSOT
       (join_audit_and_se_mid(0) at both resume emit sites)
  AC3  require_effect hard deny on resume_had_mismatch unchanged (reason
       strings untouched; capture_security_correlated_audit still joins)
  AC4  Soft/Restricted-observe (no production defaults) keeps the legacy
       mid=1 observe stamp when nothing is live
  AC5  session-revoke no-op family untouched; tests live in the mandate
       family; no invent test; no docs/design

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


def _rows(must, must_count, must_not) -> list[str]:
    fails: list[str] = []

    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    _read("tests/serve/test_resume_session_revoke.cpp")
    build = _read("build.py")

    # AC1/AC2: both resume emit sites join the SSOT mid.
    must_count("auto mid = typed_audit::join_audit_and_se_mid(0);", "AC1/AC2 join x2", fm, 2)
    must_count("if (!typed_audit::production_defaults_active() && mid == 0)", "AC4 observe stamp x2", fm, 2)
    must("Issue #3670", "AC1 cite", fm)

    # AC1: the #3594 phantom coercion is gone from the resume path.
    must_not("epoch != 0 ? epoch : static_cast<std::uint64_t>(1)", "AC1 phantom coercion removed", fm)

    # AC3: reason strings unchanged; correlated capture still joins.
    must_count('"isolation-deny:fiber-principal-mismatch"', "AC3 reason x2", fm, 2)
    must_count(
        'capture_security_correlated_audit(\n                mid, "fiber-principal-mismatch", mid',
        "AC3 capture join",
        fm,
        1,
    )

    # AC5: session-revoke no-op family untouched.
    must("ac3434_3_session_revoke_on_resume", "AC5 revoke test", test)
    must("3670 AC1", "AC5 mandate AC1", test)
    must("3670 AC2", "AC5 mandate AC2", test)
    must("3670 AC3", "AC5 mandate AC3", test)
    must("3670 AC4", "AC5 mandate AC4", test)
    must("apply_production_audit_defaults", "AC5 production arm", test)
    must("g_tls_boundary_audit_mid = 777", "AC2 live-join probe", test)
    if _read("tests/compiler/test_issue_3670.cpp") or _read("tests/issues/test_issue_3670.cpp"):
        fails.append("AC5: test_issue_3670.cpp present (forbidden per #81934)")
    if _read("docs/design/3670-resume-mid-join.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_resume_mid_join_3670", "AC5 build.py", build)
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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    fails = _rows(must, must_count, must_not)

    if fails:
        print("FAIL #3670 resume_mid_join:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3670 resume_mid_join: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    good = (
        "auto mid = typed_audit::join_audit_and_se_mid(0);\n"
        "if (!typed_audit::production_defaults_active() && mid == 0)\n"
        "isolation-deny:fiber-principal-mismatch\n"
    )
    bad = "const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);\n"
    must("auto mid = typed_audit::join_audit_and_se_mid(0);", "self join", good)
    must("if (!typed_audit::production_defaults_active() && mid == 0)", "self observe", good)
    if "epoch != 0 ? epoch : static_cast<std::uint64_t>(1)" in good:
        failures.append("self: good sample must not carry the phantom")
    if "epoch != 0 ? epoch : static_cast<std::uint64_t>(1)" not in bad:
        failures.append("self: stripped sample must carry the phantom")
    print("SELF-TEST OK: check_resume_mid_join_3670 row helpers" if not failures else f"SELF-TEST FAIL: {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
