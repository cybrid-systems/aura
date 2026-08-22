#!/usr/bin/env python3
"""Issue #3253: instance-repair SOLVED must drain residual dirty.

Production try_instance_repair_before_full reindexes unmapped
INSTANCE/SUBTYPE (densify/steal remount) and refuses finally-SOLVED
while unprocessed dirty remains. drain_pending_full_solve_before_commit
treats dirty_count_ residual (empty pending/locality) as a #3031 face.
Soft/Off: no extra scan. Additive schema-3253; reuse #3031 residual.

Contract (one row per AC):
  AC1  production repair SOLVED + injected dirty → drain rejects
  AC2  stamp gate dirty residual; force_reason 16 / #3031 face
  AC3  remount-incomplete INSTANCE reindexed before repair
  AC4  Soft skip reindex / extra escalate
  AC5  IR/JIT still consults live commit_readiness
  AC6  extend test_solve_delta_unresolved_export; no invent / docs/design

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("Issue #3253", "AC1 cite", impl)
    must("try_instance_repair_before_full", "AC1 repair", impl)
    must("handoff_locality_residual_to_pending", "AC1 merge", impl)
    must("ac3253_1_production_repair_solved_injected_dirty_rejects", "AC1 test", t)

    must("force_reason=*/16", "AC2 stamp", mb)
    must("dirty_count_ == 0", "AC2 drain dirty", impl)
    must("pending_full_solve_residual", "AC2 face", aud)
    must("ac3253_2_stamp_gate_dirty_residual", "AC2 test", t)

    must("Constraint::INSTANCE", "AC3 INSTANCE", impl)
    must("Constraint::SUBTYPE", "AC3 SUBTYPE", impl)
    must("drop_var_to_constraints_entry_for_test", "AC3 hook", ixx)
    must("ac3253_3_densify_remount_reindex", "AC3 test", t)

    must("AC4: zero cost", "AC4", impl)
    must("ac3253_4_soft_zero_extra", "AC4 test", t)

    must("ir_typed_entry_commit_readiness_ok", "AC5 ir", aud)
    must("linear_move_drop_elision_ok", "AC5 linear", aud)
    must("ac3253_5_ir_jit_commit_readiness", "AC5 test", t)

    must_key("schema-3253", "AC6 schema", q)
    must_key("repair-solved-residual-wired", "AC6 wired", q)
    must_key("schema-3031", "AC6 3031", q)
    must("check_repair_solved_residual_3253", "AC6 build", build)
    must("ac3253_6_source_and_linter", "AC6 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3253.cpp").is_file():
        fails.append("AC6: test_issue_3253.cpp present (forbidden #81967)")
    if _read("docs/design/3253-repair-solved-residual.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3253 repair_solved_residual:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3253 repair_solved_residual: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
