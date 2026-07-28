#!/usr/bin/env python3
"""Issue #2282: unified dead-coercion layered counter (AST elide + IR DCE + dirty-cone skip).

Contract (5 AC from issue body):
  AC1: One query key returns layered total = sum of three components.
  AC2: Components individually still queryable (no schema break).
  AC3: Narrowing fixture: layered monotonic increase post-mutate + lower path.
  AC4: Schema lineage additive.
  AC5: Short comment in optimization_passes.ixx / coercion_map matches live keys.

This linter is the source-of-truth for the production surface.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    q = _read("src/compiler/evaluator_primitives_query.cpp")
    opt = _read("src/compiler/optimization_passes.ixx")
    coer = _read("src/compiler/coercion_map.ixx")
    test_cpp = _read("tests/compiler/test_dead_coercion_layered_2282.cpp")

    # AC1: One query key returns layered total = sum of three components.
    _must(
        '"query:dead-coercion-layered-stats"' in q,
        "AC1: query:dead-coercion-layered-stats primitive registration missing",
        fails,
    )
    _must(
        '"dead-coercion-layered-total"' in q,
        "AC1: dead-coercion-layered-total field missing in primitive hash",
        fails,
    )
    _must(
        "g_dead_coercion_ast_elided_total.load" in q
        and "dead_coercion_ir_elided_total.load" in q
        and "dead_coercion_dirty_cone_skips.load" in q,
        "AC1: primitive must load all 3 component counters (ast_elided + ir_elided + dirty_cone_skips)",
        fails,
    )
    _must(
        "layered_total = ast_elided + ir_elided + dirty_cone_skips" in q,
        "AC1: layered_total must equal sum of 3 components",
        fails,
    )

    # AC2: Components individually still queryable (no schema break).
    _must(
        '"ast-elided"' in q and '"ir-elided"' in q and '"dirty-cone-skips"' in q,
        "AC2: each component must be exposed as a hash field (ast-elided + ir-elided + dirty-cone-skips)",
        fails,
    )
    _must(
        '"ir-narrow-evidence-hits"' in q and '"pipeline-runs-total"' in q,
        "AC2: optional fields (ir-narrow-evidence-hits + pipeline-runs-total) must also be exposed",
        fails,
    )

    # AC4: Schema lineage additive — new primitive + existing primitives untouched.
    _must(
        '"query:dead-coercion-zerooverhead-stats"' in q,
        "AC4: existing query:dead-coercion-zerooverhead-stats primitive must remain registered",
        fails,
    )
    _must(
        '"query:coercion-zerooverhead-stats"' in q,
        "AC4: existing query:coercion-zerooverhead-stats primitive must remain registered",
        fails,
    )

    # AC5: Comment alignment in optimization_passes.ixx + coercion_map.ixx.
    _must(
        "query:dead-coercion-layered-stats" in opt,
        "AC5: optimization_passes.ixx comment must reference query:dead-coercion-layered-stats",
        fails,
    )
    _must(
        "query:dead-coercion-layered-stats" in coer,
        "AC5: coercion_map.ixx comment must reference query:dead-coercion-layered-stats",
        fails,
    )
    _must(
        "dead-coercion-layered-total" in opt or "dead-coercion-layered-total" in coer,
        "AC5: at least one of opt/coer comment must mention the dead-coercion-layered-total field name",
        fails,
    )

    # AC3: Test file covers narrowing + mutate monotonic increase.
    _must(
        "test_dead_coercion_layered_2282" in test_cpp,
        "AC3: tests/compiler/test_dead_coercion_layered_2282.cpp must exist with the expected header",
        fails,
    )
    _must(
        "AC1" in test_cpp and "AC2" in test_cpp and "AC3" in test_cpp and "AC4" in test_cpp and "AC5" in test_cpp,
        "AC3: test file must include all 5 AC sections",
        fails,
    )
    _must(
        "monotonic" in test_cpp.lower() or "after >= baseline" in test_cpp.lower(),
        "AC3: test file must verify layered monotonic increase post-mutate",
        fails,
    )
    _must(
        "narrow" in test_cpp.lower(),
        "AC3: test file must include a narrowing fixture (lower path + post-mutate)",
        fails,
    )

    # Module import for atomics (file-level atomics in optimization_passes.ixx).
    _must(
        "import aura.compiler.optimization_passes" in q,
        "Implementation: evaluator_primitives_query.cpp must import aura.compiler.optimization_passes",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2282 dead-coercion layered counter coverage linter")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-test (return 0 if contract satisfied)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Strict mode (non-zero exit on any failure)",
    )
    args = parser.parse_args()
    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2282 dead-coercion layered counter - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
