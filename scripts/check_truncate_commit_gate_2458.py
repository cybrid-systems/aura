#!/usr/bin/env python3
"""Issue #2458: truncate-commit Soft observe / Hard full-solve-or-reject.

Contract:
  AC1 Soft observe counters + gate allow path
  AC2 Hard full-solve recover/reject
  AC3 incomplete blame under HARD
  AC4 happy path no extra solve
  AC5 schema-2458 + wiring + lineage keys

Exit 0 = all rows satisfied.
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
    impl = _read("src/compiler/type_checker_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_truncate_commit_gate_2458.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2458", "AC1", etc)
    must("truncate_commit_observe_total", "AC1", aud)
    must("commit_ok_after_delta_snapshot", "AC1", etc)
    must("2458 AC1", "AC1", test)

    must("truncate_commit_hard_enabled", "AC2", aud)
    must("AURA_TRUNCATE_COMMIT_HARD", "AC2", aud)
    must("truncate_commit_full_solve_recover_total", "AC2", aud)
    must("truncate_commit_reject_total", "AC2", aud)
    must("2458 AC2", "AC2", test)

    must("2458 AC3", "AC3", test)
    must("note_full_solve_cleared_truncation", "AC3", ixx)

    must("2458 AC4", "AC4", test)
    must("commit_ok_after_delta_snapshot", "AC4", impl)

    must("schema-2458", "AC5", q)
    must("truncate-commit-observe-total", "AC5", q)
    must("truncate-commit-reject-total", "AC5", q)
    must("truncate-commit-full-solve-recover-total", "AC5", q)
    must("truncate-commit-hard-wired", "AC5", q)
    must("schema-2308", "AC5", q)
    must("schema-2277", "AC5", q)
    must("check_truncate_commit_gate_2458", "gate", build)
    must("cmd_truncate_commit_gate_coverage", "gate", build)
    must("test_truncate_commit_gate_2458", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: truncate-commit gate #2458 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
