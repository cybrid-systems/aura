#!/usr/bin/env python3
"""Issue #3171: steal/densify/cross-eval restamp complete-clear.

linear_fast_path_ok stays the only predicate. Production steal-complete /
densify-success / restamp sites must (a) clear keyed escape summaries
for the eval identity and (b) advance invalidate_gen so IR Move/Drop
cannot elide on a pre-event green stamp. Soft: existing early-outs.

  AC1 Production restamp/handoff sites clear + invalidate
  AC2 After clear, linear_fast_path_ok is false until green rebind
  AC3 Soft / Off: unified_restamp early-out preserved (no extra lock)
  AC4 Reuse #2507 / #3063 counters; no new public query key
  AC5 Extend existing linear / steal suites; no invent / docs
  AC6 This linter + build.py wire

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

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
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gate = _read("src/compiler/ownership_escape_lowering_gate.h")
    low = _read("src/compiler/lowering_linear_types_impl.cpp")
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    esc = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    steal = _read("tests/compiler/test_escape_gate_steal_densify_clear.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    q = read_query_prims()
    build = _read("build.py")

    # AC1 sites
    must("kLinearFastPathStealDensifyClearCompleteIssue = 3171", "AC1 stamp", tma)
    must("Issue #3171", "AC1 tma cite", tma)
    must("clear_escape_move_elision_gate_for_eval", "AC1 unified_restamp clear", efm)
    must("UnifiedRestampSite::StealComplete", "AC1 steal restamp", efm)
    must("UnifiedRestampSite::Densify", "AC1 densify restamp", efm)
    must("invalidate_fast_path_before_steal_densify_restamp", "AC1 densify relocate", mb)
    must("had_moving_densify = compact_r.moved_live_objects", "AC1 densify relocate site", mb)
    must("note_escape_gate_clear_on_densify", "AC1 Phase-5 #2507 preserved", mb)
    must("note_escape_gate_clear_on_steal", "AC1 steal-complete #2507 preserved", efm)
    must("#3171", "AC1 lowering", low)

    densify_pos = mb.find("had_moving_densify = compact_r.moved_live_objects")
    if densify_pos < 0:
        fails.append("AC1: densify relocate assignment missing")
    else:
        window = mb[densify_pos : densify_pos + 1800]
        if "invalidate_fast_path_before_steal_densify_restamp" not in window:
            fails.append("AC1: densify relocate window missing invalidate_gen bump")
        if "#3171" not in window:
            fails.append("AC1: densify relocate window missing #3171 cite")

    restamp_pos = efm.find("Evaluator::unified_restamp_after_boundary")
    if restamp_pos < 0:
        fails.append("AC1: unified_restamp definition missing")
    else:
        window = efm[restamp_pos : restamp_pos + 2500]
        if "clear_escape_move_elision_gate_for_eval" not in window:
            fails.append("AC1: unified_restamp window missing keyed escape clear")
        if "invalidate_fast_path_before_steal_densify_restamp" not in window:
            fails.append("AC1: unified_restamp window missing invalidate_gen")
        if "skipped_extra = true" not in window:
            fails.append("AC3: unified_restamp Soft early-out missing")

    # AC2 SSOT
    must("linear_fast_path_ok", "AC2 SSOT", tma)
    must("g_rehydrate_miss_invalidate_gen", "AC2 reuse gen", tma)
    must("ac3171_1_prod_clear_blocks_elide", "AC2 test", persist)
    must("3171 AC1: Move/Drop cannot skip", "AC2 elide", persist)

    # AC3 Soft
    must("Soft already returned above", "AC3 cite", efm)
    must("ac3171_2_soft_zero_extra", "AC3 test", persist)
    must("3171 AC2: no gen bump", "AC3 gen", persist)

    # AC4 reuse + no new public query
    must("g_linear_escape_gate_steal_clear_total", "AC4 steal counter", gate)
    must("g_steal_densify_success_invalidate_total", "AC4 invalidate total", tma)
    must_key("schema-3171", "AC4 schema", q)
    must_key("linear-fast-path-steal-densify-clear-complete-wired", "AC4 wired", q)
    must("schema-2507", "AC4 lineage 2507", obs)
    must_key("schema-3063", "AC4 lineage 3063", q)
    if "query:steal-densify-clear-complete" in q or "query:linear-fast-path-steal-densify" in q:
        fails.append("AC4: new top-level query key (forbidden)")

    # AC5 / AC6 tests + wire
    must("ac3171_hermetic_clear_and_invalidate", "AC5 escape", esc)
    must("ac3171_restamp_sites_clear", "AC5 steal-clear", steal)
    must("ac3171_health_schema", "AC5 health", health)
    must("check_linear_fast_path_clear_on_restamp_3171", "AC6 build", build)
    must("cmd_linear_fast_path_clear_on_restamp_3171", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3171.cpp").is_file():
        fails.append("AC5: test_issue_3171.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3171-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3171 steal/densify restamp complete-clear — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
