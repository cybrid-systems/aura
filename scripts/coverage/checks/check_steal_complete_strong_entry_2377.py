#!/usr/bin/env python3
"""Issue #2377: force single steal-complete entry (no weak legacy under production).

Contract:
  AC1 Production multi-worker → strong steal-complete only (no residual-less path)
  AC2 Strong entry order: Panic clear → residual → LayoutStamp → linear/outermost
  AC3 Soft/sandbox weak no-op / null → metric; production aborts
  AC4 Zero cost when residual 0 / stamp unset (documented relaxed loads)
  AC5 Tests + query + gate wire

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

    wc = _read("src/serve/worker.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    gh = _read("src/core/gc_hooks.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    cmake = _read("CMakeLists.txt")
    bp = _read("build.py")

    # AC1 production contract in worker
    must("call_steal_complete", "AC1", wc)
    must("steal_snapshot_soft_production_locked", "AC1", wc)
    must("Issue #2377", "AC1", wc)
    must("std::abort()", "AC1", wc)
    must("bump_steal_complete_entry_missing_total", "AC1", wc)

    # AC1/AC3 fiber_bridge weak production-aware
    must("aura_evaluator_on_steal_complete", "AC3", fb)
    must("steal_snapshot_soft_production_locked", "AC3", fb)
    must("Issue #2377", "AC3", fb)
    must("std::abort()", "AC3", fb)
    must("bump_steal_complete_entry_missing_total", "AC3", fb)

    # AC2 transaction order documented + residual + stamp in strong body
    must("steal-complete transaction", "AC2", fm)
    must("Issue #2377", "AC2", fm)
    must("clear_gc_defer_for_evaluator", "AC2", fm)
    must("force_clear_residual_defer_for_evaluator", "AC2", fm)
    must("has_resume_layout_stamp", "AC2", fm)
    must("aura_evaluator_probe_linear_on_steal", "AC2", fm)

    # AC4 zero-cost notes
    must("zero cost when", "AC4", fm.lower())
    must("defer_reasons_snapshot()", "AC4", fm)

    # AC5 counter + query + test + gate
    must("g_steal_complete_entry_missing_total", "AC5", gh)
    must("steal_complete_entry_missing_total", "AC5", gh)
    must("schema-2377", "AC5", q)
    must("steal-complete-entry-missing-total", "AC5", q)
    must("steal-complete-strong-required-wired", "AC5", q)
    must("test_steal_complete_strong_entry", "AC5", cmake)
    must("cmd_steal_complete_strong_entry_coverage", "AC5", bp)
    must("check_steal_complete_strong_entry_2377.py", "AC5", bp)
    must("AC1:", "AC5", test)
    must("Issue #2377", "AC5", test)

    # Production path must not document legacy residual-less as default
    if "Falls back to probe_linear + outermost when weak/null" in wc:
        fails.append("AC1: worker still documents residual-less fallback as default")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2377 steal-complete strong entry — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
