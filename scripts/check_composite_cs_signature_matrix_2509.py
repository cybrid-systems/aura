#!/usr/bin/env python3
"""Issue #2509: symmetric expected_partial ↔ commit_cs_has_work matrix.

Contract:
  AC1: expected + empty → hard-miss (#2345 retained)
  AC2: expected + has_work → SDO entered; no vacuous SOLVED skip
  AC3: !expected + !has_work → structural OK
  AC4: !expected + has_work → unexpected_cs_work observe + solve
  AC5: source-cite + schema-2509; unit test covers all 4 cells

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    etc = _read("src/compiler/evaluator_typecheck.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    ixx = _read("src/compiler/type_checker.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_composite_cs_signature_matrix_2509.cpp")
    cmake = _read("CMakeLists.txt")
    gate = _read("build.py")

    # AC1 — #2345 empty hard-miss retained + matrix entry
    must("composite_empty_cs_hard_reject_enabled", "AC1", etc)
    must("composite_commit_empty_cs_hard_miss_total", "AC1", etc)
    must("expected_partial", "AC1", etc)
    must("AC1", "AC1", test)
    must("apply_production_audit_defaults", "AC1", test)

    # AC2 — expected + has_work must enter SDO
    must("composite_commit_expected_has_work_total", "AC2", aud)
    must("composite_commit_expected_has_work_total", "AC2", etc)
    must("composite_commit_sdo_entered_total", "AC2", etc)
    must("sdo_entered", "AC2", etc)
    must("require_sdo", "AC2", etc)
    must("AC2", "AC2", test)

    # AC3 — structural vacuous
    must("AC3", "AC3", test)
    must("commit_cs_has_work", "AC3", ixx)
    must("txn_dirty", "AC3", etc)

    # AC4 — unexpected CS work
    must("composite_commit_unexpected_cs_work_total", "AC4", aud)
    must("composite_commit_unexpected_cs_work_total", "AC4", etc)
    must("AC4", "AC4", test)

    # AC5 — schema + wiring + test target
    must("schema-2509", "AC5", q)
    must("composite-commit-unexpected-cs-work-total", "AC5", q)
    must("composite-commit-expected-has-work-total", "AC5", q)
    must("composite-commit-sdo-entered-total", "AC5", q)
    must("composite-cs-signature-matrix-wired", "AC5", q)
    must("schema-2509", "AC5", mut)
    must("schema-2345", "AC5", q)
    must("Issue #2509", "AC5", etc)
    must("test_composite_cs_signature_matrix_2509", "AC5", cmake)
    must("composite_cs_signature_matrix_wired", "AC5", aud)
    must("check_composite_cs_signature_matrix_2509", "AC5", gate)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2509 composite CS signature matrix — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
