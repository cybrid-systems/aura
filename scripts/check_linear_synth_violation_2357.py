#!/usr/bin/env python3
"""Issue #2357: Phase-1 linear Move/Drop first-class synthesize violation.

  AC1: note_linear_synth_violation + can_move in synthesize_flat_move
  AC2: valid mark path + escape gate retained
  AC3: production_defaults / strict hard vs Warning soft
  AC4: post_mutation_invariant + #2108 retained
  AC5: metrics + query schema-2357 + tests + gate

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

    tci = _read("src/compiler/type_checker_impl.cpp")
    tch = _read("src/compiler/type_checker.ixx")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_linear_synth_violation_2357.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("note_linear_synth_violation", "AC1", tci)
    must("note_linear_synth_violation", "AC1", tch)
    must("can_move", "AC1", tci)
    must("set_node_error", "AC1", tci)
    must("synthesize_flat_move", "AC1", tci)
    must("ac1_double_move_first_class", "AC1", test)
    must("Issue #2357", "AC1", tci)

    # AC2
    must("ownership_env_.mark", "AC2", tci)
    must("ac2_valid_move_and_escape_gate", "AC2", test)

    # AC3
    must("production_defaults_active", "AC3", tci)
    must("ErrorKind::Warning", "AC3", tci)
    must("ErrorKind::TypeError", "AC3", tci)
    must("ac3_production_hard_policy", "AC3", test)

    # AC4
    must("validate_ownership", "AC4", tci)
    must("ac4_post_mutation_and_2108", "AC4", test)

    # AC5
    must("linear_synth_violation_total", "AC5", met)
    must("linear_synth_hard_fail_total", "AC5", met)
    must("linear_synth_violation_total", "AC5", fields)
    must("schema-2357", "AC5", q)
    must("linear-synth-violation-total", "AC5", q)
    must("test_linear_synth_violation_2357", "AC5", cmake)
    must("check_linear_synth_violation_2357", "AC5", build)
    must("cmd_linear_synth_violation_coverage", "AC5", build)
    must("ac5_query_and_source", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2357 linear synth violation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
