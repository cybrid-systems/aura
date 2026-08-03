#!/usr/bin/env python3
"""Issue #2595: unify densify success gate (pin ∧ untracked ∧ panic_residual).

Coverage gate: presence-checks for the new unified gate axes + Phase 5
driver wiring + panic state source-cite. Mirrors
`check_audit_mid_fallback_slo_2594.py` / `check_parallel_isolation_level_2400.py`
style.

Contract:
  AC6 DensifyConsistencyReport has untracked_ok + panic_residual_ok axes
  AC7 force_reason priority pin > untracked > panic_residual > legacy
  AC8 g_densify_unified_gate_fail_total additive schema key + reset helper
  AC9 Phase 5 driver wires new axes from baseline captures + bumps
     unified fail counter in !overall_ok() block
  AC10 panic state source-cite in src/core/gc_hooks.h
       (g_gc_defer_pending_panic_depth + gc_deferred_for_evaluator)

Exit 0 = all rows satisfied.
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

    hdr = _read("src/core/densify_consistency_report.h")
    driver = _read("src/compiler/evaluator_mutation_boundary.cpp")
    arena_ixx = _read("src/core/arena.ixx")
    gc = _read("src/core/gc_hooks.h")
    test = _read("tests/core/test_moving_densify_fail_closed_2495.cpp")
    build = _read("build.py")

    # Header (axis extension + counter + helpers).
    must("bool untracked_ok = true;", "hdr", hdr)
    must("bool panic_residual_ok = true;", "hdr", hdr)
    must(
        "return pin_ok && untracked_ok && panic_residual_ok && linear_ok && type_ok &&",
        "hdr",
        hdr,
    )
    must('if (!untracked_ok)\n            return "untracked";', "hdr", hdr)
    must('if (!panic_residual_ok)\n            return "panic_residual";', "hdr", hdr)
    must('if (v == "untracked")\n        return "untracked";', "hdr", hdr)
    must('if (v == "panic_residual")\n        return "panic_residual";', "hdr", hdr)
    must("g_densify_unified_gate_fail_total", "hdr", hdr)
    must("bump_densify_unified_gate_fail_total", "hdr", hdr)
    must("densify_unified_gate_fail_total", "hdr", hdr)
    must("reset_densify_unified_gate_for_test", "hdr", hdr)
    must("g_densify_unified_gate_fail_total.store(0", "hdr", hdr)
    must("Issue #2595", "hdr", hdr)

    # Phase 5 driver wiring.
    must("untracked_baseline", "driver", driver)
    must("panic_depth_baseline", "driver", driver)
    must("g_moving_untracked_external_roots_total.load", "driver", driver)
    must("g_gc_defer_pending_panic_depth.load", "driver", driver)
    must("densify_consistency.untracked_ok", "driver", driver)
    must("densify_consistency.panic_residual_ok", "driver", driver)
    must("gc_deferred_for_evaluator(static_cast<void*>(ev_))", "driver", driver)
    must("bump_densify_unified_gate_fail_total", "driver", driver)
    must("Issue #2595", "driver", driver)

    # Arena (counter source for untracked axis).
    must("g_moving_untracked_external_roots_total", "arena", arena_ixx)
    must("moving_incomplete_remap", "arena", arena_ixx)
    must("untracked_kept_count", "arena", arena_ixx)

    # Panic state source-cite (gc_hooks.h).
    must("g_gc_defer_pending_panic_depth", "gc", gc)
    must("gc_deferred_for_evaluator", "gc", gc)
    must(
        "inline bool gc_deferred_for_evaluator(void* evaluator_id)",
        "gc",
        gc,
    )

    # Test additions.
    must("Issue #2595", "test", test)
    must("ac6_unify_axes_in_report", "test", test)
    must("ac7_force_reason_priority", "test", test)
    must("ac8_unified_gate_fail_counter", "test", test)
    must("ac9_phase5_driver_wiring", "test", test)
    must("ac10_panic_state_source_cite", "test", test)
    must("bool untracked_ok = true;", "test", test)
    must("bool panic_residual_ok = true;", "test", test)
    must(
        "return pin_ok && untracked_ok && panic_residual_ok && linear_ok && type_ok &&",
        "test",
        test,
    )
    must("g_densify_unified_gate_fail_total", "test", test)
    must("bump_densify_unified_gate_fail_total", "test", test)
    must("reset_densify_unified_gate_for_test", "test", test)
    must("g_moving_untracked_external_roots_total", "test", test)
    must("g_gc_defer_pending_panic_depth", "test", test)
    must("gc_deferred_for_evaluator", "test", test)

    # build.py wiring.
    must("cmd_densify_unified_gate_2595_coverage", "build", build)
    must("check_densify_unified_gate_2595", "build", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} densify-unified-gate (#2595) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2595 densify unified gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
