#!/usr/bin/env python3
"""Issue #3122: Guard abort dual topology restore is Agent-visible.

Production exception after Guard acquire returns make_merr
error="topology-restore" reason="restored". Acquire-fail stays on_fail.
Soft keeps existing on_fail shape. Hard restore unchanged.

Contract:
  AC1 Production throw after topology write → structured topology-restore
  AC2 Acquire-fail / isolation / hygiene remain distinct
  AC3 Soft path keeps on_fail (no new mandatory shape)
  AC4 Source-cite boundary + Guard helpers + one mutate entry; no docs/design/
  AC5 Extend test_mutation_rollback_coverage; no test_issue_3122.cpp

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

    gh = _read("src/compiler/mutation_guard_helpers.hh")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    comp = _read("src/compiler/evaluator_primitives_compile.cpp")
    t = _read("tests/compiler/test_mutation_rollback_coverage.cpp")
    build = _read("build.py")

    must("Issue #3122", "AC1 helper", gh)
    must("kTopologyRestoreStructuredIssue = 3122", "AC1 stamp", gh)
    must('kTopologyRestoreErrorKind = "topology-restore"', "AC1 error kind", gh)
    must("topology_restore_abort_result", "AC1 helper call", gh)
    must("topology_restore_abort_result", "AC1 impl", bound)
    must("make_merr", "AC1 make_merr", bound)
    must("3122 AC1: error=topology-restore", "AC1 test marker", t)

    must("return on_fail;", "AC2 acquire-fail", gh)
    must("3122 AC2", "AC2 test", t)
    must("not topology-restore", "AC2 comment", gh)

    must("Soft/Off: keep caller on_fail", "AC3 helper", gh)
    must("3122 AC3: Soft still #f", "AC3 test", t)

    must("Issue #3122", "AC4 boundary", bound)
    must("abort_after_insert_child_for_test", "AC4 test hook", bound)
    must("Issue #3122", "AC4 mutate", mut)
    must("Issue #3122", "AC4 compile comment", comp)
    must("3122 AC4: helper cites", "AC4 test cites", t)
    must("check_topology_restore_structured_3122", "AC5 build.py", build)
    must("test_topology_restore_structured_3122", "AC5 test fn", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3122.cpp").is_file():
        fails.append("AC5: test_issue_3122.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3122-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3122 Guard abort structured topology-restore — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
