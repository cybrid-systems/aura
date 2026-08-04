#!/usr/bin/env python3
"""Issue #2514: unify linear_synth_hard_fail with MutationBoundary audit exit.

Contract:
  AC1 production synth hard-fail → force-rollback; no Success; skip soft recovery
  AC2 Soft Warning → no forced rollback solely from synth flag
  AC3 escape-only / post-mutate defense-in-depth retained
  AC4 counter ownership + stable query keys (no double-count)
  AC5 source-cite single exit decision table + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    tci = _read("src/compiler/type_checker_impl.cpp")
    tch = _read("src/compiler/type_checker.ixx")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_linear_synth_boundary_authority_2514.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2514", "AC1", etc)
    must("deny_if_linear_synth_hard_fail", "AC1", etc)
    must("deny_if_linear_synth_hard_fail", "AC1", eixx)
    must("linear_synth_hard_fail_pending", "AC1", emb)
    must("linear-synth-hard-fail", "AC1", etc)
    must("last_linear_synth_hard_fail", "AC1", tch)
    must("last_linear_synth_hard_fail", "AC1", tci)
    must("linear_synth_boundary_force_rollback_total", "AC1", aud)
    must("linear_synth_boundary_skip_recovery_total", "AC1", etc)
    must("ac1_production_force_rollback", "AC1", test)

    # AC2
    must("ErrorKind::Warning", "AC2", tci)
    must("production_defaults_active", "AC2", tci)
    must("ac2_soft_warning_no_force", "AC2", test)

    # AC3
    must("post_mutation_invariant_check", "AC3", tci + tch)
    must("ac3_escape_defense_in_depth", "AC3", test)

    # AC4
    must("linear_synth_boundary_force_rollback_total", "AC4", aud)
    must("schema-2514", "AC4", q)
    must("linear-synth-boundary-force-rollback-total", "AC4", q)
    must("linear-synth-authority-unified", "AC4", q)
    must("schema-2514", "AC4", mut)
    must("Do NOT bump linear_invariant_fail", "AC4", etc)
    must("schema-2357", "AC4", q)
    must("ac4_counter_ownership", "AC4", test)

    # AC5
    must("decision table", "AC5", etc.lower() + eixx.lower())
    must("Issue #2514", "AC5", emb)
    must("ac5_decision_table", "AC5", test)
    must("test_linear_synth_boundary_authority_2514", "AC5", cmake)
    must("check_linear_synth_boundary_authority_2514", "AC5", build)
    must("cmd_linear_synth_boundary_authority_coverage", "AC5", build)

    # Phase-1 retained
    must("note_linear_synth_violation", "retain", tci)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2514 linear_synth ↔ MutationBoundary authority — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
