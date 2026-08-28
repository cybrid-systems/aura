#!/usr/bin/env python3
"""Issue #3333: provenance_ok mid join is per contributing grant.

#2707 fail-closed mid is kept. Residual: ANY live grant mid mismatch
poisoned the whole tenant. Now only grants that contribute `required`
bits (has_effect) join mid/epoch/fiber. required==None keeps the
query-path "check all live grants" contract.

Contract (one row per AC):
  AC1  grantA(Render, mid=5) + grantB(Mutate, mid=9); Mutate@9 allows
  AC2  only Mutate bound_mid=5 vs check mid=9 denies + provenance_mismatch
  AC3  Restricted/Strict + prov.mutation_id==0 still denies (#2707)
  AC4  Soft/Off zero-mid skip unchanged
  AC5  stolen skip; #3142 session/steal lineage
  AC6  extend test_capability_single_use_consume; linter after #2707;
       no invent / no docs/design

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

    cap = _read("src/core/capability_model.hh")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    lint2707 = _read("scripts/coverage/checks/check_mid_join_fail_closed_2707.py")
    lint3126 = _read("scripts/coverage/checks/check_capability_admin_fence_3126.py")
    build = _read("build.py")

    must("kProvenanceContributingMidIssue = 3333", "AC1 stamp", cap)
    must("required != Effect::None && !has_effect(g.effects, required)", "AC1 filter", cap)
    must("provenance_ok_locked(tenant, prov, required)", "AC1 check path", cap)
    must("ac3333_1_unrelated_grant_does_not_poison", "AC1 test", test)

    must("g.bound_mutation_id != prov.mutation_id", "AC2 strict eq", cap)
    must("ac3333_2_true_mismatch_still_denies", "AC2 test", test)
    must("capability_provenance_mismatch_total", "AC2 metric", cap)

    must("fail_closed_mid", "AC3 #2707", cap)
    must("prov.mutation_id == 0", "AC3 zero mid", cap)
    must("ac3333_3_zero_mid_fail_closed", "AC3 test", test)
    must("Issue #2707", "AC3 lineage", lint2707)

    must("skip-when-zero", "AC4 Soft skip", cap)
    must("ac3333_4_soft_zero_skip", "AC4 test", test)

    must("g.revoked || g.stolen", "AC5 stolen skip", cap)
    must("Issue #3142", "AC5 #3142", cap)
    must("ac3333_5_stolen_skip_and_source", "AC5 test", test)

    must("check_provenance_contributing_mid_3333", "AC6 build.py", build)
    must("check_mid_join_fail_closed_2707", "AC6 #2707 linter wired", build)
    must("ac3333_6_source_and_linter", "AC6 test", test)
    must("provenance_ok(TenantId tenant, const EffectProvenance& prov", "AC6 3126 prefix", lint3126)
    prev = build.find("check_mid_join_fail_closed_2707")
    ours = build.find("check_provenance_contributing_mid_3333")
    if ours < 0:
        fails.append("AC6: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #2707")
    if (ROOT / "tests" / "core" / "test_issue_3333.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3333.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3333-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3333 contributing-grant mid join — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
