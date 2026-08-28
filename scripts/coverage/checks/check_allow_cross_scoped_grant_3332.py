#!/usr/bin/env python3
"""Issue #3332: Restricted allow_cross_tenant is scoped to cross_grants.

#3010 closed the write gate (TenantAdmin/wildcard to set the flag). Residual:
check_boundary_ex returned true on allow_cross_tenant, skipping same/cross
tenant, cross_grants, and ref provenance. Restricted/Strict now fall through
to the grant + provenance walk. Soft/Off keep the zero-cost short-circuit.

Contract (one row per AC):
  AC1  #3010 write gate retained (Restricted without TenantAdmin cannot set flag)
  AC2  Restricted + allow_cross, no cross_grants[T1→T2] → foreign Mutate deny
       (cap_deny) + IsolationDeny
  AC3  grant_cross_tenant covering bits + allow_cross → allow; insufficient bits deny
  AC4  stamped foreign ref + allow_cross without current→ref grant → prov_deny
  AC5  Soft/Off keep allow_cross short-circuit (no extra lock/counter)
  AC6  Extend test_tenant_isolation_enforcement; linter after #3010; no invent /
       no docs/design

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
    sec = _read("src/compiler/evaluator_security.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    lint3010 = _read("scripts/coverage/checks/check_allow_cross_tenant_admin_3010.py")
    build = _read("build.py")

    must("kAllowCrossScopedGrantIssue = 3332", "AC1 stamp", iso)
    must("Issue #3010", "AC1 #3010 write gate", iso)
    must("allow-cross-needs-tenant-admin", "AC1 write deny reason", sec)
    must("3332 AC1: Restricted without TenantAdmin cannot set flag", "AC1 test", test_iso)

    must("allow_cross_tenant && !(strict || sandbox_restricted)", "AC2 gated short-circuit", iso)
    must("cross_grant_bits(cur, target)", "AC2 grant walk retained", iso)
    must("3332 AC2: foreign Mutate without grant denies", "AC2 test", test_iso)
    must("allow_cross without grant denies", "AC2 rewritten bypass case", test_iso)
    if "return true;  // full isolation short-circuit" in iso:
        fails.append("AC2: unconditional allow_cross short-circuit still present")

    must("allow_cross + grant allows", "AC3 rewritten allow case", test_iso)
    must("3332 AC3: Mutate grant + allow_cross allows", "AC3 test", test_iso)
    must("3332 AC3: insufficient bits still deny", "AC3 bits", test_iso)

    must("cross_grant_bits(cur, ref_tenant)", "AC4 provenance walk retained", iso)
    must("3332 AC4: foreign ref without current→ref grant denies", "AC4 test", test_iso)

    must("the zero-cost bypass", "AC5 comment", iso)
    must("Soft/Off keep", "AC5 Soft/Off gate", iso)
    must("3332 AC5: Off allow_cross still short-circuits", "AC5 test", test_iso)
    must("Issue #3010", "AC5 #3010 linter retained", lint3010)

    must("3332 AC6: A without grant still denies", "AC6 dual-eval", test_iso)
    must("check_allow_cross_scoped_grant_3332", "AC6 build.py", build)
    must("check_allow_cross_tenant_admin_3010", "AC6 #3010 linter still wired", build)
    prev = build.find("check_allow_cross_tenant_admin_3010")
    ours = build.find("check_allow_cross_scoped_grant_3332")
    if ours < 0:
        fails.append("AC6: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3010")
    if (ROOT / "tests" / "core" / "test_issue_3332.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3332.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3332-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3332 allow_cross scoped to cross_grants — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
