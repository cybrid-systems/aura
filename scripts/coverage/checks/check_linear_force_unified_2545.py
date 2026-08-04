#!/usr/bin/env python3
"""Issue #2545: unify linear hard-fail decision entry (force_linear_rollback).

Contract:
  AC1 production synth → force_linear_rollback; skip soft recovery
  AC2 synth early-exit no linear_invariant_fail double-count
  AC3 post-mutate-only / CrossBatchEscape retained through unified entry
  AC4 Soft Warning does not force
  AC5 clean path zero extra force counters
  AC6 all boundary/hard-gate/composite sites call force_linear_rollback;
      schema-2545 + linter + test + gate wiring

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

    etc = _read("src/compiler/evaluator_typecheck.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_linear_force_unified.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("force_linear_rollback", "AC1", eixx)
    must("LinearForceAuthority", "AC1", eixx)
    must("classify_linear_force", "AC1", eixx)
    must("force_linear_rollback", "AC1", etc)
    must("SynthHardFail", "AC1", etc)
    must("force_linear_rollback", "AC1", emb)
    must("linear_synth_boundary_force_rollback_total", "AC1", etc)
    must("ac1_synth_force_via_unified", "AC1", test)

    # AC2
    must("Do NOT bump linear_invariant_fail", "AC2", etc)
    must("no double-count", "AC2", aud.lower() + etc.lower())
    must("ac2_no_double_count", "AC2", test)

    # AC3
    must("PostMutateLinear", "AC3", etc)
    must("CrossBatchEscape", "AC3", etc)
    must("last_post_mutate_linear_fail", "AC3", eixx)
    must("note_post_mutate_linear_fail", "AC3", etc)
    must("ac3_post_mutate_only", "AC3", test)

    # AC4
    must("Soft Warning", "AC4", etc)
    must("ac4_soft_warning", "AC4", test)

    # AC5
    must("zero-cost clean path", "AC5", etc)
    must("ac5_clean_zero_cost", "AC5", test)

    # AC6 — all sites + schema + gate
    must("force_linear_rollback", "AC6", emb)
    must("force_linear_rollback", "AC6", etc)
    must("return force_linear_rollback", "AC6", etc)  # deny_if alias
    must("composite_txn_commit", "AC6", etc)
    must("Issue #2545", "AC6", eixx)
    must("Issue #2545", "AC6", etc)
    must("linear_force_unified_2545", "AC6", aud)
    must("schema-2545", "AC6", q)
    must("linear-force-unified", "AC6", q)
    must("schema-2545", "AC6", mut)
    must("schema-2514", "AC6", q)  # lineage retained
    must("test_linear_force_unified", "AC6", cmake)
    must("check_linear_force_unified_2545", "AC6", build)
    must("cmd_linear_force_unified_coverage", "AC6", build)
    must("ac6_source_and_schema", "AC6", test)

    # Phase-1 retained
    must("deny_if_linear_synth_hard_fail", "retain", etc)
    must("linear_synth_hard_fail_pending", "retain", etc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2545 force_linear_rollback unified entry — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
