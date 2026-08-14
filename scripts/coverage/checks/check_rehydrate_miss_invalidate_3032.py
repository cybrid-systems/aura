#!/usr/bin/env python3
"""Issue #3032: densify/steal rehydrate-miss invalidates linear_fast_path + deopt.

Production/Full miss stamps Reject then advances invalidate_gen so
linear_fast_path_ok() is false and hot Move/Drop cannot keep a pre-miss
green stamp. Soft observe. Quiet (no miss) is zero extra.

Contract:
  AC1 Production miss → !linear_fast_path_ok + force deopt/revalidate
  AC2 Soft: observe only; no gen bump
  AC3 Quiet (no miss): no extra counters
  AC4 Successful rehydrate binds fingerprint before green stamp
  AC5 schema-3032 + residual keys; #2981/#2910/#3030 preserved
  AC6 extend persist-rehydrate + escape-elision + health; no invent / docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    ixx = _read("src/compiler/type_checker.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = read_query_prims()
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    esc = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("invalidate_fast_path_on_rehydrate_miss", "AC1", tma)
    must("g_rehydrate_miss_invalidate_gen", "AC1", tma)
    must("invalidate_fast_path_on_rehydrate_miss", "AC1 fence", ixx)
    must("invalidate_fast_path_on_rehydrate_miss", "AC1 densify", mb)
    must("invalidate_fast_path_on_rehydrate_miss", "AC1 steal", efm)
    must("aura_jit_walk_active_closures", "AC1 steal deopt", efm)
    must("aura_aot_record_deopt_on_steal", "AC1 steal deopt", efm)
    must("ac3032_1_prod_miss_invalidates_fast_path", "AC1", persist)

    must("ac3032_2_soft_observe_only", "AC2", persist)
    must("g_rehydrate_miss_invalidate_observe_total", "AC2", tma)

    must("ac3032_3_quiet_zero_cost", "AC3", persist)

    must("note_rehydrate_success_bind", "AC4", tma)
    must("note_rehydrate_success_bind", "AC4 fence", ixx)
    must("note_rehydrate_success_bind", "AC4 steal", efm)
    must("ac3032_4_success_bind", "AC4", persist)

    must("ac3032_5_schema", "AC5", persist)
    must("ac3032_health_schema", "AC5", health)
    must_key("schema-3032", "AC5", q)
    must_key("rehydrate-miss-invalidate-total", "AC5", q)
    must_key("rehydrate-miss-invalidate-observe-total", "AC5", q)
    must_key("rehydrate-miss-force-deopt-total", "AC5", q)
    must_key("rehydrate-miss-success-bind-total", "AC5", q)
    must_key("rehydrate-miss-invalidate-wired", "AC5", q)
    must("kRehydrateMissInvalidateIssue", "AC5", tma)

    must("ac3032_6_source_and_linter", "AC6", persist)
    must("ac3032_hermetic_invalidate", "AC6", esc)
    must("check_rehydrate_miss_invalidate_3032", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3032.cpp").is_file():
        fails.append("AC6: test_issue_3032.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3032-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3032 rehydrate-miss invalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
