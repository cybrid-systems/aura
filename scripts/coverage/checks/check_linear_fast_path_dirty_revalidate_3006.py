#!/usr/bin/env python3
"""Issue #3006: !linear_fast_path_ok forces dirty-root revalidate.

Residual of #2964: Production/Full outermost success re-evaluates the
predicate after Phase 1; if false, enforce_linear_boundary_consistency
(no Quiet / render_fast skip). Soft observe. Production never elides
under a false predicate. escape/densify/depth stay hard blockers.

Contract:
  AC1 Production !ok → ForceRevalidate + dirty-root walk
  AC2 Production never try_skip / elide when !ok
  AC3 late re-eval after Phase 1; render_fast cannot skip
  AC4 Soft observe only
  AC5 schema-3006 + lineage #2964/#2899
  AC6 extend test_escape_move_elision_gate; linter; no docs/design/; no test_issue_3006

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    low = _read("src/compiler/lowering_linear_types_impl.cpp")
    esc = _read("src/compiler/ownership_escape_lowering_gate.h")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    build = _read("build.py")

    # AC1
    must("kLinearFastPathDirtyRevalidateIssue", "AC1", aud)
    must("g_linear_fast_path_dirty_revalidate_total", "AC1", aud)
    must("enforce_linear_boundary_consistency", "AC1", mb)
    must("g_linear_fast_path_dirty_revalidate_total", "AC1", mb)
    must("ac3006_1_production_dirty_root", "AC1", t)

    # AC2
    must("g_linear_fast_path_elide_blocked_production_total", "AC2", aud)
    must("never elides under a false predicate", "AC2", ir)
    must("ac3006_2_no_elide_under_false", "AC2", t)

    # AC3
    must("linear_fast_path_maybe_force_dirty_revalidate", "AC3", mb)
    must("late re-eval after Phase 1", "AC3", mb)
    must("aura_linear_fast_path_depth_or_densify_block", "AC3", low)
    must("aura_linear_fast_path_depth_or_densify_block", "AC3", hooks)
    must("aura_linear_fast_path_depth_or_densify_block", "AC3", esc)
    must("ac3006_3_late_reeval_and_render_fast", "AC3", t)

    # AC4
    must("SoftObserve", "AC4", aud)
    must("ac3006_4_soft_observe", "AC4", t)

    # AC5
    must("schema-3006", "AC5", q)
    must("linear-fast-path-dirty-revalidate-total", "AC5", q)
    must("linear-fast-path-late-reeval-total", "AC5", q)
    must("linear-fast-path-elide-blocked-production-total", "AC5", q)
    must("schema-2964", "AC5 lineage", q)
    must("schema-2899", "AC5 lineage", q)
    must("ac3006_5_schema_lineage", "AC5", t)

    # AC6
    must("check_linear_fast_path_dirty_revalidate_3006", "AC6", build)
    must("cmd_linear_fast_path_dirty_revalidate_3006_coverage", "AC6", build)
    must("ac3006_6_linter_no_design", "AC6", t)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3006*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_3006.cpp").is_file():
        fails.append("tests/compiler/test_issue_3006.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3006 linear_fast_path dirty-root revalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
