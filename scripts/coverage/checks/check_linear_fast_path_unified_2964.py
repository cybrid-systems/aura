#!/usr/bin/env python3
"""Issue #2964: unified linear_fast_path_ok gate + force revalidate.

Symmetric to #2899 IR Move/Drop elision: single pure predicate
  proof.fresh && linear_ok && depth==0 && !escape && !densify_pending.
!ok on outermost MutationBoundary success under production/Full forces
dirty-root revalidate; Soft observe-only; quiet zero when ok.

Contract:
  AC1 linear_fast_path_ok pure; IR try_skip uses it
  AC2 ForceRevalidate under production when !ok; Soft observe; Quiet when ok
  AC3 mid-boundary / escape / densify each independently disable
  AC4 Zero extra revalidate when ok
  AC5 schema-2964 + force keys; preserve #2899/#2263
  AC6 Source-cite boundary/lowering/escape/audit; extend test_escape_move_elision_gate;
      linter + build.py; no docs/design/*

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
    ir = _read("src/compiler/ir_executor_impl.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    esc = _read("src/compiler/ownership_escape_lowering_gate.h")
    low = _read("src/compiler/lowering_linear_types_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    build = _read("build.py")

    # AC1
    must("linear_fast_path_ok", "AC1", aud)
    must("#2964", "AC1", aud)
    must("linear_fast_path_ok", "AC1", ir)
    must("linear_ir_fastpath_try_skip", "AC1", aud)
    must("ac2964_1_unified_predicate", "AC1", t)

    # AC2
    must("linear_fast_path_boundary_exit_action", "AC2", aud)
    must("ForceRevalidate", "AC2", aud)
    must("SoftObserve", "AC2", aud)
    must("g_linear_fast_path_force_revalidate_total", "AC2", aud)
    must("linear_fast_path_boundary_exit_action", "AC2", mb)
    must("record_revalidate_hit", "AC2", mb)
    must("ac2964_2_force_revalidate_production", "AC2", t)

    # AC3
    must("mid MutationBoundary", "AC3", aud)
    must("escape gate arm", "AC3", aud)
    must("densify-pending arm", "AC3", aud)
    must("ac2964_3_independent_arms", "AC3", t)

    # AC4
    must("Quiet", "AC4", aud)
    must("ac2964_4_soft_and_quiet", "AC4", t)

    # AC5
    must("schema-2964", "AC5", q)
    must("linear-fast-path-force-revalidate-total", "AC5", q)
    must("schema-2899", "AC5 lineage", q)
    must("schema-2263", "AC5 lineage", q)

    # AC6
    must("2964", "AC6", esc)
    must("2964", "AC6", low)
    must("check_linear_fast_path_unified_2964", "AC6", build)
    must("cmd_linear_fast_path_unified_2964_coverage", "AC6", build)
    must("linear-fast-path-unified-2964", "AC6", build)
    must("ac2964_6_source_cite", "AC6", t)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2964-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2964.cpp").is_file():
        fails.append("tests/compiler/test_issue_2964.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2964 linear_fast_path_ok unified gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
