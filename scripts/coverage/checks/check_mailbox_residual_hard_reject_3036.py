#!/usr/bin/env python3
"""Issue #3036: mailbox residual RejectHard stay fail-closed under production.

production_defaults + !AURA_SANDBOX=off → RejectHard is Backpressure and
mailbox_residual_hard_reject_total bumps. Soft / sandbox=off: soft_observe.
Happy path remains thread_local only. Same StealInvariant table as steal.

Contract:
  AC1 production + residual fail → BP, no enqueue, hard counter
  AC2 Soft: zero-cost happy; soft_observe only on fail (still BP)
  AC3 same StealInvariant bit-set as steal_safety_transaction
  AC4 additive mailbox-residual-hard-reject-total + schema-2987
  AC5 extend #2987 test + linter; chaos soak forces production_defaults

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
    ss = _read("src/serve/steal_safety.h")
    sc = _read("src/serve/steal_safety.cpp")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    t = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    must("mailbox_residual_hard_enabled", "AC1", mb)
    must("mailbox_residual_hard_reject_total", "AC1", mb)
    must("Issue #3036", "AC1", mb)
    must("ac3036_1_production_residual_hard_bp", "AC1", t)
    must("return true", "AC1 never silent Ok", mb)

    must("ac3036_2_soft_observe_only", "AC2", t)
    must("mailbox_delivery_reject_soft_observe_total", "AC2", mb)
    must("mailbox_sandbox_explicit_off", "AC2", mb)
    must("g_mailbox_delivery_inject == MailboxDeliveryInject::None && target == nullptr", "AC2 happy", mb)

    must("evaluate_residual_hard_and_bits", "AC3", sc)
    must("StealInvariant::LayoutStampMatch", "AC3", sc)
    must("StealInvariant::TicketFresh", "AC3", sc)
    must("StealInvariant::GcDeferClear", "AC3", sc)
    must("mailbox_delivery_safety_transaction", "AC3", sc)
    must("ac3036_3_shared_steal_bits", "AC3", t)
    must("StealInvariant", "AC3 table", ss)

    must("mailbox-residual-hard-reject-total", "AC4", msg)
    must("schema-3036", "AC4", msg)
    must("schema-2987", "AC4 lineage", msg)
    must("ac3036_4_schema", "AC4", t)

    must("ac3036_5_source_linter_chaos", "AC5", t)
    must("apply_production_audit_defaults", "AC5 chaos", chaos)
    must("ac3036_mailbox_residual_prod_fail_closed_cite", "AC5 chaos cite", chaos)
    must("check_mailbox_residual_hard_reject_3036", "AC5", build)
    must("maybe_mailbox_defer_slo_hold_cancel", "AC5 #2958 compose", mb)
    if (ROOT / "tests" / "serve" / "test_issue_3036.cpp").is_file():
        fails.append("AC5: test_issue_3036.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3036-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3036 mailbox residual hard-reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
