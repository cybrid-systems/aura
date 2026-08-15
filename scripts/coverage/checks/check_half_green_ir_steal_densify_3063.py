#!/usr/bin/env python3
"""Issue #3063: steal/densify SUCCESS invalidate-before-restamp.

Production/Full steal or densify restamp advances the existing
rehydrate_miss_invalidate_gen *before* node/pin restamp so
linear_fast_path_ok() is false and in-flight IR Move cannot elide
on a pre-restamp green proof. Soft: zero extra atomics. No second
predicate — linear_fast_path_ok stays SSOT.

  AC1 Production success restamp → !linear_fast_path_ok + no IR elide
  AC2 Soft: helper no-op, no gen / no new counters
  AC3 linear_fast_path_ok remains the single predicate
  AC4 extend persist-rehydrate + escape-elision + health; no invent / docs

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
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    q = read_query_prims()
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    esc = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1
    must("invalidate_fast_path_before_steal_densify_restamp", "AC1", tma)
    must("Issue #3063", "AC1 cite", tma)
    must("invalidate_fast_path_before_steal_densify_restamp", "AC1 restamp", efm)
    must("UnifiedRestampSite::StealComplete", "AC1 steal site", efm)
    must("UnifiedRestampSite::Densify", "AC1 densify site", efm)
    must("aura_jit_walk_active_closures", "AC1 jit deopt", efm)
    must("Issue #3063", "AC1 IR", ir)
    must("ac3063_1_prod_success_blocks_elide", "AC1 test", persist)
    must("3063 AC1: Move/Drop cannot skip", "AC1 elide", persist)

    # AC2 Soft
    must("Soft/Off: no extra atomics", "AC2 cite", tma)
    must("ac3063_2_soft_zero_extra", "AC2 test", persist)
    must("3063 AC2: no gen bump", "AC2 gen", persist)

    # AC3 SSOT
    must("linear_fast_path_ok", "AC3 SSOT", tma)
    must("g_rehydrate_miss_invalidate_gen", "AC3 reuse gen", tma)
    must("3063 AC3: SSOT predicate", "AC3 test", persist)
    if "proof_invalidate_gen" in tma and "g_proof_invalidate_gen" in tma:
        fails.append("AC3: second invalidate gen model (must reuse rehydrate_miss_invalidate_gen)")

    # AC4 wiring
    must("ac3063_hermetic_success_invalidate", "AC4 escape", esc)
    must("ac3063_health_schema", "AC4 health", health)
    must("check_half_green_ir_steal_densify_3063", "AC4 build", build)
    must("cmd_half_green_ir_steal_densify_3063", "AC4 cmd", build)
    must_key("schema-3063", "AC4 query", q)
    must_key("steal-densify-success-invalidate-total", "AC4 query total", q)
    must_key("steal-densify-success-invalidate-wired", "AC4 query wired", q)
    if (ROOT / "tests" / "compiler" / "test_issue_3063.cpp").is_file():
        fails.append("AC4: test_issue_3063.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3063-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")
    if "query:half-green" in q or "query:steal-densify-success-invalidate" in q:
        fails.append("AC4: new top-level query key (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3063 steal/densify success invalidate-before-restamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
