#!/usr/bin/env python3
"""Issue #2939: solve_delta reverify = bounded dep-closure (O(affected)).

Contract (one row per AC):
  AC1 BFS/dep-closure over var_to_constraints_ + UF reps; small dirty
      keeps delta_reverify_closure_cap_hit flat when |closure| ≤ limit
  AC2 Cap hit → pending_full_solve_roots_ residual (no silent starve)
  AC3 Empty seeds (incl. pending) → early return zero cost
  AC4 CONFLICT / escalate_if_production preserved
  AC5 Additive closure metrics + schema-2939; #2146/#2356 preserved
  AC6 coverage linter + src/-aligned suite; no docs/design/

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    h = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    test = _read("tests/compiler/test_adaptive_reverify_limit.cpp")
    build = _read("build.py")

    # ── AC1: dep-closure collection ──
    must("#2939", "AC1", impl)
    must("#2939", "AC1", ixx)
    must("var_to_constraints_", "AC1", impl)
    must("pending_full_solve_roots_", "AC1 seed", impl)
    must("bounded dependency closure", "AC1", impl)
    must("ac2939_1_small_dirty_closure_no_cap", "AC1", test)

    # ── AC2: cap → pending ──
    must("delta_reverify_closure_cap_hit_total", "AC2", impl)
    must("pending_full_solve_roots_.insert", "AC2", impl)
    must("ac2939_2_cap_hit_pending", "AC2", test)

    # ── AC3: empty zero cost ──
    must("pending_full_solve_roots_.empty()", "AC3", impl)
    must("ac2939_3_soft_empty_zero", "AC3", test)

    # ── AC4: fail-closed preserved ──
    must("escalate_if_production", "AC4", ixx)
    must("SolveResult::CONFLICT", "AC4", impl)
    must("ac2939_4_conflict_timeout_preserved", "AC4", test)

    # ── AC5: metrics + lineage ──
    must("delta_reverify_closure_nodes_total", "AC5", h)
    must("delta_reverify_closure_edges_total", "AC5", h)
    must("delta_reverify_closure_cap_hit_total", "AC5", h)
    must("delta_reverify_closure_nodes_total", "AC5 fields", fields)
    must("delta-reverify-closure-nodes-total", "AC5", q)
    must("delta-reverify-closure-cap-hit-total", "AC5", q)
    must("schema-2939", "AC5", q)
    must("schema-2146", "AC5", q)
    must("schema-2356", "AC5", q)
    must("delta_reverify_expand_total", "AC5", h)
    must("ac2939_5_metrics_lineage", "AC5", test)

    # ── AC6 ──
    must("ac2939_1_small_dirty_closure_no_cap", "AC6", test)
    must("ac2939_6_linter_and_no_design", "AC6", test)
    must("check_solve_delta_dep_closure_2939", "AC6", build)
    must("cmd_solve_delta_dep_closure_2939", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2939-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2939.cpp").is_file():
        fails.append("AC6: test_issue_2939.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2939 solve_delta dep-closure reverify — O(affected) BFS + pending residual + schema-2939")
    return 0


if __name__ == "__main__":
    sys.exit(main())
