#!/usr/bin/env python3
"""Issue #3801: IsolationDeny / production deny SE mid joins TypedMid.

IsolationDeny record_audit must use TypedMid-then-epoch (same resolver
family as require_effect / join_audit_and_se_mid). Production deny arms
must not mint phantom mid=1 via ``epoch ?: 1``. Soft/Off observe mid=1
(#2493) stays on Soft arms only.

Contract (one row per AC):
  AC1  under Guard TypedMid≠epoch, IsolationDeny SE mid == TypedMid
  AC2  Restricted+MT epoch=0 deny SE mid=0 (no phantom 1)
  AC3  Soft/Off observe stamp contract unchanged
  AC4  cite-first linter forbids production epoch?:1 outside Soft arms;
       no new query key

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    iso = _read("src/core/workspace_isolation.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")

    # ── AC1 IsolationDeny TypedMid join ──
    must("Issue #3801", "AC1 cite iso", iso)
    must("aura_isolation_deny_se_mid", "AC1 hook call", iso)
    must("aura_isolation_deny_se_mid", "AC1 hook def", sec)
    must("production_deny_se_mid", "AC1 join family", sec)
    must("join_audit_and_se_mid", "AC1 join in helper", sec)
    must("3801 AC1", "AC1 test", test)
    must("IsolationDeny SE mid == TypedMid", "AC1 test title", test)

    # ── AC2 epoch=0 → mid=0 ──
    must("production_deny_se_mid", "AC2 helper", sec)
    must("3801 AC2", "AC2 test", test)
    must("no phantom mid=1", "AC2 phantom guard", test)
    # record_audit must not coerce to 1
    if "epoch != 0 ? epoch : 1" in iso or "epoch ?: 1" in iso:
        fails.append("AC2: workspace_isolation.hh still has epoch?:1 coercion")

    # ── AC3 Soft/Off unchanged ──
    must("Soft / Off keeps the mid=1 observe stamp", "AC3 Soft cite", sec)
    must("epoch != 0 ? epoch : static_cast<std::uint64_t>(1)", "AC3 Soft arm kept", sec)
    must("prov.mutation_id == 0 && !force_bind", "AC3 Soft session gate", sec)
    must("3801 AC3", "AC3 test", test)

    # ── AC4: forbid production epoch?:1 outside Soft arms; no query key ──
    must("Issue #3801", "AC4 cite sec", sec)
    must("check_isolation_deny_mid_join_3801", "AC4 build.py", build)
    must("Issue #3801", "AC4 build cite", build)
    must_not("schema-3801", "AC4 no schema key", prim)
    must_not("issue-3801", "AC4 no issue key", prim)
    for rel in ("tests/core/test_issue_3801.cpp", "tests/compiler/test_issue_3801.cpp"):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3801-*.md"):
            fails.append(f"AC4: {f.relative_to(ROOT)} exists — forbidden per #1655")

    # Production phantom mint scan: every `epoch != 0 ? epoch : static_cast<...>(1)`
    # in evaluator_security.cpp must sit inside a Soft-observe arm (the
    # fiber-principal Soft fallback that production overrides via join).
    phantom = "epoch != 0 ? epoch : static_cast<std::uint64_t>(1)"
    soft_window_ok = 0
    for m in re.finditer(re.escape(phantom), sec):
        start = max(0, m.start() - 200)
        window = sec[start : m.end() + 600]
        # Soft arm: #3462 production override follows the Soft observe stamp.
        if ("Issue #3462" in window or "production_defaults_active()" in window) and (
            "join_audit_and_se_mid" in window or "Soft" in window
        ):
            soft_window_ok += 1
            continue
        fails.append(
            "AC4: production epoch?:1 mid mint outside Soft arm near: "
            + sec[max(0, m.start() - 60) : m.end() + 20].replace("\n", " ")
        )
    if soft_window_ok < 1:
        fails.append("AC4: expected ≥1 Soft-observe epoch?:1 arm retained")

    # Bare `const auto mid = epoch != 0 ? epoch : 1` production form must be gone.
    must_not(
        "const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);",
        "AC4 no bare production phantom",
        sec,
    )

    if fails:
        print(f"Issue #3801 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3801 IsolationDeny / deny SE mid join — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
