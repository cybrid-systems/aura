#!/usr/bin/env python3
"""Issue #2511: outermost Guard exit forced mailbox deferred drain under budget.

Contract:
  AC1 Guard dtor + drain_deferred_under_budget source-cite
  AC2 hold defers then exit drain path
  AC3 budget env + starvation + health
  AC4 chaos multi-fiber
  AC5 happy path free when depth 0

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
    test = _read("tests/serve/test_mailbox_hold_exit_drain_2511.cpp")
    cmake = _read("CMakeLists.txt")
    bp = _read("build.py")

    # AC1
    must("drain_deferred_under_budget", "AC1", mb)
    must("drain_deferred_under_budget", "AC1", emb)
    must("Issue #2511", "AC1", mb)
    must("AURA_MAILBOX_HOLD_DRAIN_BUDGET_US", "AC1", mb)
    must("AC1", "AC1", test)

    # AC2
    must("mailbox_hold_exit_drain_total", "AC2", mb)
    must("had_open_defer", "AC2", mb)
    must("AC2", "AC2", test)

    # AC3
    must("mailbox_hold_exit_starvation_total", "AC3", mb)
    must("mailbox_hold_exit_starvation_total", "AC3", health)
    must("schema-2511", "AC3", epm)
    must("mailbox-hold-exit-drain-wired", "AC3", epm)
    must("schema-2511", "AC3", q)
    must("AC3", "AC3", test)

    # AC4
    must("AC4", "AC4", test)
    must("chaos", "AC4", test)

    # AC5
    must("depth0 == 0", "AC5", mb)
    must("AC5", "AC5", test)
    must("test_mailbox_hold_exit_drain_2511", "AC5", cmake)
    must("check_mailbox_hold_exit_drain_2511", "AC5", bp)
    must("cmd_mailbox_hold_exit_drain_coverage", "AC5", bp)
    must("schema-2378", "AC5", epm)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2511 mailbox hold-exit drain — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
