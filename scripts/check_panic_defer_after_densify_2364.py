#!/usr/bin/env python3
"""Issue #2364: PanicCheckpoint residual × densify closed-loop coverage.

  AC1: audit_panic_defer_after_densify free path (Soft / no densify / no panic)
  AC2: residual without checkpoint → force clear
  AC3: live checkpoint without defer → re-arm
  AC4: AURA_PANIC_CONTRACT=hard + counters
  AC5: Phase 5 wire + query schema-2364 + tests + gate

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

    gh = _read("src/core/gc_hooks.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_panic_defer_after_densify_2364.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 free path
    must("audit_panic_defer_after_densify", "AC1", gh)
    must("free_path", "AC1", gh)
    must("ac1_soft_free", "AC1", test)

    # AC2 clear orphan
    must("g_panic_defer_after_densify_cleared_total", "AC2", gh)
    must("force_clear_all_gc_defer_for_evaluator", "AC2", gh)
    must("ac2_clear_orphan_after_densify", "AC2", test)

    # AC3 rearm
    must("g_panic_defer_after_densify_rearmed_total", "AC3", gh)
    must("try_arm_gc_defer_pending_panic_for", "AC3", gh)
    must("ac3_rearm_for_live_checkpoint", "AC3", test)

    # AC4 hard
    must("AURA_PANIC_CONTRACT", "AC4", gh)
    must("panic_contract_hard_enabled", "AC4", gh)
    must("g_panic_defer_after_densify_hard_fail_total", "AC4", gh)
    must("std::abort()", "AC4", gh)

    # AC5 wire + query + gate
    must("Issue #2364", "AC5", mb)
    must("audit_panic_defer_after_densify", "AC5", mb)
    must("panic_defer_after_densify_total", "AC5", met)
    must("schema-2364", "AC5", q)
    must("issue-2364", "AC5", q)
    must("panic-defer-after-densify-wired", "AC5", q)
    must("test_panic_defer_after_densify_2364", "AC5", cmake)
    must("check_panic_defer_after_densify_2364", "AC5", build)
    must("cmd_panic_defer_after_densify_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    # Steal residual path retained (#2314)
    must("force_clear_residual_defer_for_evaluator", "AC5", gh)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2364 PanicCheckpoint residual × densify — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
