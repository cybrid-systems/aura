#!/usr/bin/env python3
"""Issue #2987: mailbox delivery residual hard-AND (steal StealInvariant table).

Contract (one row per AC):
  AC1 push/fanout never enqueue while residual GcDefer / LayoutStamp /
     Ticket is inconsistent, even if depth/held looks safe.
  AC2 Production RejectHard → BP + hard counter; Soft still BP + soft_observe.
  AC3 Happy path (no inject, no target, boundary idle) — thread_local only.
  AC4 Steal and mailbox share evaluate_residual_hard_and_bits / StealInvariant.
  AC5 Additive schema-2987; #2849/#2903/#2551 keys non-regressing.
  AC6 Inject residual/stale ticket → BP; clear → Ok. No docs/design / invent.

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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} of {n!r}, found {c}")

    mb = _read("src/serve/multi_fiber_mailbox.h")
    ss = _read("src/serve/steal_safety.h")
    sc = _read("src/serve/steal_safety.cpp")
    fh = _read("src/serve/fiber.h")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    t = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # AC1 — gate sites + never enqueue
    must("Issue #2987", "AC1", mb)
    must("note_mailbox_delivery_safety", "AC1", mb)
    must_count("note_mailbox_delivery_safety", "AC1 sites", mb, 3)
    must("mailbox_delivery_safety_transaction", "AC1", mb)
    must("never enqueue", "AC1", mb)
    must("return PushStatus::Backpressure", "AC1 BP", mb)
    must("ac2987_1_inject_residual_bp", "AC1", t)

    # AC2 — Soft vs production
    must("mailbox_delivery_reject_hard_total", "AC2", mb)
    must("mailbox_delivery_reject_soft_observe_total", "AC2", mb)
    must("is_mutate_mailbox_strict", "AC2", mb)
    must("ac2987_2_soft_still_bp", "AC2", t)

    # AC3 — happy path zero extra atomics
    must("thread_local", "AC3 inject", ss)
    must("no extra atomics", "AC3", mb)
    must("g_mailbox_delivery_inject == MailboxDeliveryInject::None && target == nullptr", "AC3", mb)
    must("ac2987_3_happy_zero_extra", "AC3", t)

    # AC4 — shared StealInvariant table
    must("evaluate_residual_hard_and_bits", "AC4", ss)
    must("evaluate_residual_hard_and_bits", "AC4 impl", sc)
    must("StealInvariant::LayoutStampMatch", "AC4", sc)
    must("StealInvariant::TicketFresh", "AC4", sc)
    must("StealInvariant::GcDeferClear", "AC4", sc)
    must("mailbox_delivery_safety_transaction", "AC4", sc)
    must("Does NOT take the", "AC4 no steal mutex", ss)
    must("kMailboxDeliverySafetyIssue = 2987", "AC4", ss)
    must("Issue #2987", "AC4 fiber.h", fh)
    must("ac2987_4_shared_invariants", "AC4", t)

    # AC5 — query + lineage
    must("schema-2987", "AC5", msg)
    must("issue-2987", "AC5", msg)
    must("mailbox-delivery-reject-layout-stamp-total", "AC5", msg)
    must("mailbox-delivery-reject-ticket-stale-total", "AC5", msg)
    must("mailbox-delivery-reject-residual-total", "AC5", msg)
    must("mailbox-delivery-safety-wired", "AC5", msg)
    must("schema-2849", "AC5 lineage", msg)
    must("schema-2903", "AC5 lineage", msg)
    must("schema-2551", "AC5 lineage", msg)
    must("ac2987_5_query_additive", "AC5", t)

    # AC6 — inject + linter + no invent
    must("set_mailbox_delivery_inject_for_test", "AC6", ss)
    must("ac2987_6_source_and_linter", "AC6", t)
    must("check_mailbox_delivery_safety_2987", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2987-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2987.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2987.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2987 mailbox delivery residual hard-AND — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
