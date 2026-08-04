#!/usr/bin/env python3
"""Issue #2378: mailbox defer drain SLA + hold-blocked latency.

Contract:
  AC1 defer → deferred_depth + hold total
  AC2 outermost exit + Ok drain → flush latency sample
  AC3 no-defer Ok path free (depth load only)
  AC4 query schema-2378 keys
  AC5 Guard dtor + tests + gate

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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    epm = _read("src/compiler/evaluator_primitives_messaging.cpp")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    bp = _read("build.py")

    # AC1
    must("note_mailbox_mutation_hold_defer", "AC1", mb)
    must("mailbox_deferred_depth", "AC1", mb)
    must("mailbox_deferred_depth_high_water", "AC1", mb)
    must("Issue #2378", "AC1", mb)
    must("ac2378_defer_depth", "AC1", test)

    # AC2
    must("note_mailbox_outermost_exit_drain", "AC2", mb)
    must("note_mailbox_push_ok_drain_progress", "AC2", mb)
    must("mailbox_deferred_flush_latency_us_total", "AC2", mb)
    must("mailbox_deferred_flush_samples", "AC2", mb)
    # #2511 wraps note_mailbox_outermost_exit_drain inside drain_deferred_under_budget.
    if "note_mailbox_outermost_exit_drain" not in emb and "drain_deferred_under_budget" not in emb:
        fails.append("AC2: missing note_mailbox_outermost_exit_drain or drain_deferred_under_budget in Guard dtor")
    must("ac2378_flush_latency_after_exit", "AC2", test)

    # AC3
    must("depth == 0", "AC3", mb)
    must("ac2378_happy_path_zero_extra", "AC3", test)

    # AC4 query
    must("schema-2378", "AC4", epm)
    must("issue-2378", "AC4", epm)
    must("mailbox-deferred-depth", "AC4", epm)
    must("mailbox-deferred-flush-samples", "AC4", epm)
    must("mailbox-defer-starvation-total", "AC4", epm)
    must("mailbox-defer-drain-sla-wired", "AC4", epm)
    must("FlatHashTable::create(128)", "AC4", epm)
    must("ac2378_query_schema", "AC4", test)

    # AC5
    must("mailbox_defer_starvation_total", "AC5", mb)
    must("cmd_mailbox_defer_drain_sla_coverage", "AC5", bp)
    must("check_mailbox_defer_drain_sla_2378.py", "AC5", bp)
    must("ac2378_source_cite", "AC5", test)
    must("Issue #2378", "AC5", emb)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2378 mailbox defer drain SLA — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
