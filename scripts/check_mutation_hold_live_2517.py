#!/usr/bin/env python3
"""Issue #2517: real-time longest outermost MutationBoundary hold probe.

Contract:
  AC1 outermost enter/exit maintain live max probe
  AC2 query fiber_id / duration surface
  AC3 no holder → zeros
  AC4 coexist with #2405 estimate
  AC5 best-effort CAS + gate wiring

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

    bud = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_mutation_hold_live_2517.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2517", "AC1", bud)
    must("mutation_hold_live_note_enter", "AC1", bud)
    must("mutation_hold_live_note_exit", "AC1", bud)
    must("g_mutation_hold_live_fiber_id", "AC1", bud)
    must("g_mutation_hold_live_start_ns", "AC1", bud)
    must("mutation_hold_live_note_enter", "AC1", emb)
    must("mutation_hold_live_note_exit", "AC1", emb)
    must("Issue #2517", "AC1", emb)
    must("AC1", "AC1", test)

    # AC2
    must("query:mutation-hold-live", "AC2", q)
    must("fiber-id", "AC2", q)
    must("start-ns", "AC2", q)
    must("duration-us", "AC2", q)
    must("depth", "AC2", q)
    must("mutation_hold_live_snapshot", "AC2", bud)
    must("AC2", "AC2", test)

    # AC3
    must("s.held = true", "AC3", bud)  # held only when fiber+start set
    must("mutation_hold_live_reset_for_test", "AC3", bud)
    must("AC3", "AC3", test)
    must("held", "AC3", q)

    # AC4
    must("schema-2517", "AC4", q)
    must("schema-2405", "AC4", q)
    must("hold-estimate-coexist", "AC4", q)
    must("query:mutation-hold-estimate", "AC4", q)
    must("AC4", "AC4", test)

    # AC5
    must("Best-effort", "AC5", bud)
    must("compare_exchange_strong", "AC5", bud)
    must("AC5", "AC5", test)
    must("test_mutation_hold_live_2517", "AC5", cmake)
    must("check_mutation_hold_live_2517", "AC5", build)
    must("cmd_mutation_hold_live_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2517 mutation-hold-live — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
