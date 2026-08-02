#!/usr/bin/env python3
"""Issue #2551: mailbox hold-exit residual under production → hard + throttle.

Contract:
  AC1 hard counter + agent throttle under Strict/production residual
  AC2 Soft / free path zero extra; flag clear
  AC3 subsequent free drain / window close clears flag
  AC4 query schema-2551 + chaos test
  AC5 source-cite next to #2511/#2378; cmake + gate

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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    epm = _read("src/compiler/evaluator_primitives_messaging.cpp")
    health = _read("src/compiler/mutation_concurrency_health.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/serve/test_mailbox_hold_starvation_hard_2551.cpp")
    cmake = _read("CMakeLists.txt")
    bp = _read("build.py")

    # AC1
    must("Issue #2551", "AC1", mb)
    must("mailbox_hold_starvation_hard_total", "AC1", mb)
    must("agent_throttle_for_mailbox_starvation", "AC1", mb)
    must("ac1_production_hard_signal", "AC1", test)

    # AC2
    must("clear_agent_throttle_for_mailbox_starvation", "AC2", mb)
    must("ac2_soft_and_free", "AC2", test)

    # AC3
    must("AC3", "AC3", test)
    must("clear_agent_throttle_for_mailbox_starvation", "AC3", mb)

    # AC4
    must("schema-2551", "AC4", epm)
    must("agent-throttle-for-mailbox-starvation", "AC4", epm)
    must("mailbox-hold-starvation-hard-total", "AC4", epm)
    must("schema-2551", "AC4", q)
    must("mailbox_hold_starvation_hard_total", "AC4", health)
    must("ac4_chaos_and_query", "AC4", test)

    # AC5
    must("#2551", "AC5", emb)
    must("drain_deferred_under_budget", "AC5", emb)
    must("Issue #2511", "AC5", mb)
    must("test_mailbox_hold_starvation_hard_2551", "AC5", cmake)
    must("check_mailbox_hold_starvation_hard_2551", "AC5", bp)
    must("cmd_mailbox_hold_starvation_hard_coverage", "AC5", bp)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2551 mailbox hold starvation hard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
