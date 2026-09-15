#!/usr/bin/env python3
"""Issue #3808: IsolationDeny obs join — TypedMid query surfaces.

#3801 landed the record_audit mid resolver (join_audit_and_se_mid /
TypedMid-then-epoch). This issue locks the observability join AC:
under Guard TypedMid≠epoch, IsolationDeny SE must join
query:security-audit mutation-id=TypedMid and
query:evolution-audit-decision last-se-reason.

Contract (one row per AC):
  AC1–AC3  production mid join already #3801 (cite retained; Soft/Off unchanged)
  AC4      oracle: Guard TypedMid≠epoch → security-audit mid=TypedMid +
           evolution-audit-decision last-se-reason; cite-first linter;
           no new query key

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    iso = _read("src/core/workspace_isolation.hh")
    sec_prim = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # ── AC1–AC3: mid join + Soft/Off retained from #3801 ──
    must("Issue #3801", "AC1 cite iso (#3801 mid join)", iso)
    must("aura_isolation_deny_se_mid", "AC1 hook", iso)
    must("if (!denied)", "AC3 Soft/Off early return", iso)
    must("Issue #3808", "AC1–4 cite iso (#3808 obs join)", iso)

    # ── AC4: oracle + cite-first + no new query key ──
    must("3808 AC4", "AC4 oracle test marker", test)
    must("query:security-audit", "AC4 security-audit join", test)
    must("query:evolution-audit-decision", "AC4 evolution-audit-decision join", test)
    must("last-se-reason", "AC4 last-se-reason assert", test)
    must("IsolationDeny", "AC4 IsolationDeny in oracle", test)
    must("check_isolation_deny_obs_join_3808", "AC4 build.py", build)
    must("Issue #3808", "AC4 build cite", build)
    must_not("schema-3808", "AC4 no schema key", sec_prim)
    must_not("issue-3808", "AC4 no issue key", sec_prim)
    for rel in ("tests/core/test_issue_3808.cpp", "tests/compiler/test_issue_3808.cpp"):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3808-*.md"):
            fails.append(f"AC4: {f.relative_to(ROOT)} exists — forbidden per #1655")

    # Obs join comment must not regress to epoch-only mid claim.
    if "isolation-deny mutation_id is epoch, never tenant" in sec_prim:
        fails.append(
            "AC4: stale security-audit-stats comment still claims IsolationDeny mid=epoch only"
        )
    must("TypedMid-then-epoch", "AC4 stats cite TypedMid join", sec_prim)

    if fails:
        print(f"Issue #3808 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3808 IsolationDeny obs join — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
