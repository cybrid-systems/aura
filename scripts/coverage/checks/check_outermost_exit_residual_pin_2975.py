#!/usr/bin/env python3
"""Issue #2975: outermost MutationBoundary exit residual + pin_contract hard gate.

Contract (one row per AC):
  AC1 Production hard gate on every outermost Guard / Phase-5 exit
     (success + non-intentional-failure): close_residual_defer_after_exit
     + fail-closed on leftover residual or !pin_contract_held / incomplete-remap.
  AC2 Soft: observe-only residual_after_exit; never force-clear or fail.
  AC3 Happy path: residual==0 && pin held → single relaxed load, no clear.
  AC4 Steal-complete shares residual_defer_leftover predicate (#2546/#2890).
  AC5 Additive residual-after-exit-hard-fail / pin-contract-fail-on-exit /
     schema-2975; Soft residual-after-exit-total preserved.
  AC6 Tests extend test_residual_gc_defer_assert (ac2975_*); chaos soak
     production leftover==0; no docs/design/* (#1655).

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    gh = _read("src/core/gc_hooks.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_residual_gc_defer_assert.cpp")
    chaos = _read("tests/serve/test_chaos_steal_mutation_gc.cpp")
    build = _read("build.py")

    # AC1 — production hard gate + fail-closed.
    must("kOutermostExitResidualPinGateIssue = 2975", "AC1", gh)
    must("gate_outermost_exit_residual_and_pin", "AC1", gh)
    must("outermost_exit_should_fail_closed", "AC1", gh)
    must("mark_outermost_mutation_failed", "AC1", mb)
    must("gate_outermost_exit_residual_and_pin", "AC1", mb)
    must("production_force", "AC1", mb)
    must("fail_closed", "AC1", mb)
    must("ac2975_3_production_pin_fail_closed", "AC1", t)

    # AC2 — Soft observe-only.
    must("never force-clear or fail", "AC2", gh)
    must("production_force=*/false", "AC2", t)
    must("ac2975_2_soft_observe_no_fail", "AC2", t)
    must("Soft does not force-clear residual", "AC2", t)

    # AC3 — zero happy-path cost.
    must("single relaxed", "AC3", gh)
    must("happy_path", "AC3", gh)
    must("ac2975_1_happy_path_zero_cost", "AC3", t)

    # AC4 — steal shares leftover predicate.
    must("residual_defer_leftover", "AC4", gh)
    must("residual_defer_leftover", "AC4", mut)
    must("#2975", "AC4", mut)
    must("#2546", "AC4", gh)
    must("#2932", "AC4 compose", mb)

    # AC5 — additive metrics; Soft total preserved.
    must("g_residual_after_exit_hard_fail_total", "AC5", gh)
    must("g_pin_contract_fail_on_exit_total", "AC5", gh)
    must("residual_after_exit_hard_fail_total", "AC5", obs)
    must("pin_contract_fail_on_exit_total", "AC5", obs)
    must_key("residual-after-exit-hard-fail-total", "AC5", q)
    must_key("pin-contract-fail-on-exit-total", "AC5", q)
    must_key("schema-2975", "AC5", q)
    must_key("schema-2846", "AC5 preserved", q)
    must_key("residual-defer-after-exit-total", "AC5 preserved", q)

    # AC6 — tests + linter + chaos + no invent/design.
    must("ac2975_1_happy_path_zero_cost", "AC6", t)
    must("ac2975_2_soft_observe_no_fail", "AC6", t)
    must("ac2975_3_production_pin_fail_closed", "AC6", t)
    must("ac2975_4_steal_shares_leftover_predicate", "AC6", t)
    must("ac2975_5_additive_metrics", "AC6", t)
    must("ac2975_6_tests_linter_chaos", "AC6", t)
    must("check_outermost_exit_residual_pin_2975", "AC6", build)
    must("2975", "AC6", chaos)
    must("pin_contract_fail_on_exit", "AC6", chaos)
    must("residual_defer_leftover", "AC6", chaos)
    if (ROOT / "tests" / "compiler" / "test_issue_2975.cpp").is_file():
        fails.append("AC6: test_issue_2975.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2975*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2975 outermost-exit residual+pin hard gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
