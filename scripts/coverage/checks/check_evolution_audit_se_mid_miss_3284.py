#!/usr/bin/env python3
"""Issue #3284 linter — evolution-audit-decision SE match discipline.

Residual of #3114/#3280 (Agent decision correctness, P1): the
query:evolution-audit-decision SE walk only filtered by mid when the
explicit-mid arg (filt_mid) was present — on the default last-stamped path
it bound the LATEST SE row of ANY mid beside a typed hit for mid M, so a
typed Error/Rollback for M could be published with mid N's SE reason /
denied (cross-mid SE bleed). Fix: SE match discipline — when join_mid != 0
(explicit arg or default last_stamped path), only accept SE rows with
e.mutation_id == join_mid; if none, publish additive se-mid-miss=1 (mirror
typed-trail-miss) and leave last-se-reason-code=0 / empty reason /
last-se-denied=0. Schema additive only (AC4); Soft / no :durable stays zero
disk I/O (AC3).

Gate rows:
  G1  evaluator_primitives_security.cpp cites Issue #3284.
  G2  SE walk filters by join_mid (not filt_mid): the condition contains
      `join_mid != 0 && e.mutation_id != join_mid`.
  G3  se_mid_miss computed from a same-mid SE hit:
      `se_mid_miss = (join_mid != 0 && !se_mid_hit) ? 1 : 0`.
  G4  additive se-mid-miss key inserted (`insert_kv("se-mid-miss", ...)`).
  G5  additive schema/issue sentinels schema-3284 + issue-3284 present;
      existing schema/issue sentinels unchanged (no renames).
  G6  durable WAL path still gated on want_durable + production
      (AC3: Soft / no :durable zero disk I/O).
  G7  test ACs in tests/compiler/test_evolution_audit_decision_forensic.cpp
      (#3152 suite home, #81967) citing #3284.
  G8  build.py wires this linter.
  G9  no docs/design/3284-* (per #1655), no tests/issue*/test_issue_3284.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3284 evolution-audit-decision SE match discipline linter ===")
    src = read("src/compiler/evaluator_primitives_security.cpp")
    test = read("tests/compiler/test_evolution_audit_decision_forensic.cpp")
    build = read("build.py")

    must("Issue #3284" in src, "G1: evaluator_primitives_security.cpp cites Issue #3284")
    must("join_mid != 0 && e.mutation_id != join_mid" in src, "G2: SE walk filters by join_mid (not filt_mid)")
    must("se_mid_miss = (join_mid != 0 && !se_mid_hit) ? 1 : 0" in src, "G3: se_mid_miss computed from same-mid SE hit")
    must('insert_kv("se-mid-miss", se_mid_miss)' in src, "G4: additive se-mid-miss key")
    must(
        'insert_kv("schema-3284", 3284)' in src and 'insert_kv("issue-3284", 3284)' in src,
        "G5: schema-3284 + issue-3284 sentinels additive",
    )
    must(
        "if (want_durable && join_mid != 0 && production_defaults_active())" in src,
        "G6: durable WAL path still gated (AC3 zero disk I/O)",
    )
    must(
        "ac8_3284_se_mid_miss" in test and "Issue #3284" in test,
        "G7: test ACs in test_evolution_audit_decision_forensic.cpp cite #3284",
    )
    must("check_evolution_audit_se_mid_miss_3284.py" in build, "G8: build.py wires linter")

    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3284-") for p in (ROOT / "docs/design").glob("3284-*"))
    must(docs_ok, "G9a: no docs/design/3284-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3284.cpp").exists(),
        "G9b: no tests/issues/test_issue_3284.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3284 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3284 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
