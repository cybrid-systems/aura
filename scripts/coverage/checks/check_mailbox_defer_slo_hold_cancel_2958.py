#!/usr/bin/env python3
"""Issue #2958: mailbox defer-wait SLO → hold-budget cancel on outermost holder.

Contract:
  AC1 production + wait/open-age ≥ SLO → request_hold_budget_cancel on live holder
  AC2 Soft / under-SLO → no cancel; #2903 hist still updates
  AC3 one-shot arm (no cancel storms); Fiber CAS consume preserved
  AC4 additive metrics; #2903/#2726/#2947 lineage non-regressing
  AC5 source-cite + tests + linter; no invent test file
  AC6 no docs/design/*
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
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    fiber_h = _read("src/serve/fiber.h")
    gate = _read("src/orch/security_schedule_gate.h")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2958", "AC1", mb)
    must("maybe_mailbox_defer_slo_hold_cancel", "AC1", mb)
    must("aura_fiber_request_hold_budget_cancel", "AC1", mb)
    must("mailbox_defer_slo_hold_cancel_total", "AC1", mb)
    must("mutation_hold_live_snapshot", "AC1", mb)
    must("mailbox_under_boundary_wait_slo_us", "AC1", mb)
    must("aura_production_defaults_active_probe", "AC1", mb)
    must("ac2958_1_production_wait_slo_cancels_holder", "AC1", test)

    # AC2
    must("mailbox_defer_slo_soft_observe_total", "AC2", mb)
    must("ac2958_2_soft_and_under_slo", "AC2", test)
    must("note_mailbox_under_boundary_wait_sample", "AC2 hist retained", mb)

    # AC3
    must("g_mailbox_defer_slo_hold_cancel_armed", "AC3", mb)
    must("compare_exchange_strong", "AC3", mb)
    must("ac2958_3_one_shot_no_storm", "AC3", test)
    must("request_hold_budget_cancel", "AC3 #2726", fiber_h)
    must("consume_hold_budget_cancel", "AC3 #2726", fiber_h)

    # AC4
    must("schema-2958", "AC4", msg)
    must("mailbox-defer-slo-hold-cancel-total", "AC4", msg)
    must("schema-2903", "AC4 lineage", msg)
    must("kSecurityScheduleMailboxHoldSloIssue = 2947", "AC4 #2947", gate)
    must("ac2958_4_query_additive", "AC4", test)

    # AC5 / AC6
    must("check_mailbox_defer_slo_hold_cancel_2958", "AC5", build)
    must("ac2958_5_source_and_linter", "AC5", test)
    must("aura_fiber_request_hold_budget_cancel", "AC5 weak", fb)
    # Call sites
    if "maybe_mailbox_defer_slo_hold_cancel()" not in mb:
        fails.append("AC1: helper must be invoked from mailbox paths")
    if mb.count("maybe_mailbox_defer_slo_hold_cancel") < 3:
        fails.append("AC1: expect helper def + ≥2 call sites")
    if (ROOT / "tests" / "serve" / "test_issue_2958.cpp").is_file():
        fails.append("AC5: test_issue_2958.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2958-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2958 mailbox defer-SLO hold-budget cancel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
