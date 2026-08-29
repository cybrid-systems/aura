#!/usr/bin/env python3
"""Issue #3059: unify production reemit through decide_and_reemit.

Residual of cascade / BoundaryExit / reload / exhausted min-dirty calling
aura_reemit_aot_for_dirty directly (dual-track vs Registry pipeline).

  AC1  production sites go through decide_and_reemit
  AC2  cascade success stamps last_success like pipeline
  AC3  Soft / provider-not-wired: one load, no extra walk
  AC4  no new query keys / hot-path counters
  AC5  extend existing suite (#81967); no test_issue_3059.cpp;
       no docs/design/ (#1655)

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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    dirty = _read("src/compiler/service_dirty.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_hot_update_cascade_dirty_reemit.cpp")
    build = _read("build.py")

    # AC1
    must("kHotUpdateDecideAndReemitIssue = 3059", "AC1 stamp", hh)
    must("decide_and_reemit", "AC1 decl", hh)
    must("enum class ReemitReason", "AC1 reason", hh)
    must("HotUpdateRegistry::decide_and_reemit", "AC1 impl", cpp)
    impl = cpp.find("HotUpdateRegistry::decide_and_reemit")
    if impl < 0:
        fails.append("AC1: facade impl missing")
    else:
        body = cpp[impl : impl + 1800]
        if "aura_reemit_aot_for_dirty" not in body:
            fails.append("AC1: facade does not call aura_reemit_aot_for_dirty")
        # Issue #3413: last_reemit_success is stamped by
        # on_reemit_pipeline_call (candidates ∩ emit_region_mask_), not
        # a facade fallback that copies the full demoted / force_jit
        # mask on any n>0. Requiring note_reemit_success_coverage here
        # would resurrect that over-cover.
        if "note_reemit_success_coverage(" in body:
            fails.append("AC1: facade must not fallback-stamp via note_reemit_success_coverage (#3413)")
        if "skip the fallback `covered = demoted` stamp" not in body:
            fails.append("AC1: facade missing #3413 skip of full-mask coverage stamp")
    must("ReemitReason::Cascade", "AC1 cascade", dirty)
    must("decide_and_reemit", "AC1 cascade call", dirty)
    must("ReemitReason::BoundaryExit", "AC1 BoundaryExit", mb)
    must("ReemitReason::ResidualPipeline", "AC1 residual", mb)
    must("ReemitReason::ReloadRecovery", "AC1 reload", br)
    must("ReemitReason::ExhaustedMinDirty", "AC1 min-dirty", br)
    must("ReemitReason::StormClear", "AC1 storm-clear", cpp)
    must("ac3059_1_facade_source", "AC1 test", test)

    # Production call sites must not invoke the C ABI as the *entry*
    # (comments mentioning the name are fine). Check a tight window
    # around notify / auto-drain / recovery / retry bodies.
    cascade = dirty.find("void CompilerService::notify_hot_update_after_cascade_")
    if cascade < 0:
        fails.append("AC1: notify_hot_update_after_cascade_ missing")
    else:
        body = dirty[cascade : cascade + 3500]
        # The only aura_reemit_aot_for_dirty in this function should be
        # a comment next to decide_and_reemit.
        call_idx = body.find("aura_reemit_aot_for_dirty(")
        if call_idx >= 0:
            fails.append("AC1: cascade still calls aura_reemit_aot_for_dirty(")

    drain = mb.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
    if drain < 0:
        fails.append("AC1: auto-drain bumper missing")
    else:
        body = mb[drain : drain + 1600]
        if "decide_and_reemit" not in body:
            fails.append("AC1: auto-drain missing decide_and_reemit")
        if "aura_reemit_aot_for_dirty(" in body:
            fails.append("AC1: auto-drain still calls aura_reemit_aot_for_dirty(")

    recov = mb.find("void Evaluator::run_hot_update_recovery_if_needed")
    if recov < 0:
        fails.append("AC1: run_hot_update_recovery_if_needed missing")
    else:
        body = mb[recov : recov + 4500]
        if "decide_and_reemit" not in body:
            fails.append("AC1: recovery missing decide_and_reemit")
        if "aura_reemit_aot_for_dirty(" in body:
            fails.append("AC1: recovery still calls aura_reemit_aot_for_dirty(")

    retry = br.find("aura_hot_update_maybe_retry_exhausted_min_dirty")
    if retry < 0:
        fails.append("AC1: maybe_retry missing")
    else:
        body = br[retry : retry + 1800]
        if "decide_and_reemit" not in body:
            fails.append("AC1: maybe_retry missing decide_and_reemit")

    # Steal-complete must not grow a reemit drain (#2715 / #3059 non-goal).
    steal = fib.find("void Evaluator::probe_and_repin_linear_on_steal")
    if steal >= 0:
        body = fib[steal : steal + 700]
        if "decide_and_reemit" in body or "aura_reemit_aot_for_dirty(" in body:
            fails.append("AC1: steal-complete must not drain reemit")

    # AC2
    must("note_reemit_success_coverage", "AC2 coverage", cpp)
    must("ac3059_2_cascade_coverage_matches_pipeline", "AC2 test", test)
    must("last_reemit_success_region_mask", "AC2 last_success", test)

    # AC3 Soft / unwired: C ABI already no-ops; facade adds work only on n>0
    must("if (n > 0)", "AC3 success-only stamp", cpp)
    must("ac3059_3_unwired_zero_cost", "AC3 test", test)

    # AC4 — no new query keys / schema-3059
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    if "schema-3059" in mut or "schema-3059" in q:
        fails.append("AC4: new schema-3059 query key (forbidden)")
    must("ac3059_4_linter_no_invent", "AC4/5 test", test)

    # AC5
    must("check_hot_update_decide_and_reemit_3059", "AC5 build", build)
    must("cmd_hot_update_decide_and_reemit_3059", "AC5 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3059.cpp").is_file():
        fails.append("AC5: test_issue_3059.cpp present (forbidden per #81967)")
    if _read("docs/design/3059-decide-and-reemit.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3059 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3059 decide_and_reemit facade — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
