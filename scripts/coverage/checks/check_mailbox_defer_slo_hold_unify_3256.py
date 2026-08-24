#!/usr/bin/env python3
"""Issue #3256: mailbox under-boundary SLO unifies with hold-budget force path.

#2958 observed p99/SLO and requested Fiber cancel, but did not arm the
same hold-budget force_degrade / inbody poll that releases the live
outermost holder (#2701/#2720/#3254). Result: mailbox Backpressure
storm while the holder kept workspace_mtx_ until a separate poll edge.

#3256: production SLO → force_degrade(live fiber_id) then
poll_inbody_window. Soft: observe-only. Residual hard-AND (#2987)
unchanged. Reuse mailbox_defer_slo_hold_cancel_total + holder_degrade_*.
No new public query key.

Contract:
  AC1  production SLO calls force_degrade on live snapshot fiber_id
  AC2  order force_degrade then poll_inbody; no second unlock path
  AC3  mailbox_delivery_safety_transaction residual still after defer
  AC4  Soft: soft_observe only (early return before force_degrade)
  AC5  extend test_mailbox_recv_mutation_boundary; linter after #3255
  AC6  no docs/design/3256-*; no tests/issues/test_issue_3256.cpp

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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    t = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")
    hold = _read("src/compiler/mutation_hold_budget.h")

    fn = mb.find("inline void maybe_mailbox_defer_slo_hold_cancel() noexcept {")
    win = mb[fn : fn + 5000] if fn >= 0 else ""

    must("Issue #3256", "AC1 cite", win)
    must("aura_evaluator_force_degrade_outermost_holder", "AC1 force_degrade", win)
    must("mutation_hold_live_snapshot", "AC1 live snapshot", win)
    must("mailbox_defer_slo_hold_cancel_total", "AC1 reuse cancel_total", win)
    must("ac3256_1_production_slo_force_degrades_holder", "AC1 test", t)
    must("g_mutation_hold_budget_holder_degrade_total", "AC1 holder_degrade", hold)

    must("aura_hold_budget_poll_inbody_window", "AC2 poll", win)
    must("aura_fiber_request_hold_budget_cancel", "AC2 cancel retained", win)
    deg = win.find("aura_evaluator_force_degrade_outermost_holder")
    # Order on the ARM path: force_degrade precedes the arm-time inbody
    # poll (no second unlock). Anchor the poll lookup AFTER force_degrade
    # — the #3289 armed-branch re-poll (watchdog) appears textually before
    # force_degrade in the CAS-fail path and is NOT the arm poll.
    poll = win.find("aura_hold_budget_poll_inbody_window", deg) if deg >= 0 else -1
    if deg < 0 or poll < 0 or deg > poll:
        fails.append("AC2: force_degrade must precede poll_inbody_window")
    must("ac3256_2_force_path_order_no_second_unlock", "AC2 test", t)

    must("mailbox_delivery_safety_transaction", "AC3 residual", mb)
    must("note_mailbox_delivery_safety", "AC3 face", mb)
    must("ac3256_3_residual_hard_and_preserved", "AC3 test", t)

    must("mailbox_defer_slo_soft_observe_total", "AC4 soft", win)
    must("aura_production_defaults_active_probe", "AC4 Soft gate", win)
    soft_ret = win.find("mailbox_defer_slo_soft_observe_total")
    force = win.find("aura_evaluator_force_degrade_outermost_holder")
    if soft_ret < 0 or force < 0 or soft_ret > force:
        fails.append("AC4: Soft observe/return must precede force_degrade")
    must("ac3256_4_soft_observe_only", "AC4 test", t)

    must("ac3256_5_source_and_linter", "AC5 test", t)
    must("check_mailbox_defer_slo_hold_unify_3256", "AC5 build.py", build)
    nfe = build.find("check_dual_dep_graph_soft_parity_partial_3255")
    ours = build.find("check_mailbox_defer_slo_hold_unify_3256")
    if nfe < 0 or ours < 0 or ours < nfe:
        fails.append("AC5: linter must be wired in build.py AFTER #3255")
    if "schema-3256" in mb or "query:mailbox-defer-slo-3256" in mb:
        fails.append("AC5: new public query key (reuse existing counters)")

    if (ROOT / "tests" / "issues" / "test_issue_3256.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3256.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3256.cpp").is_file():
        fails.append("AC6: forbidden tests/serve/test_issue_3256.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3256-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3256 mailbox_defer_slo_hold_unify:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3256 mailbox_defer_slo_hold_unify: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
