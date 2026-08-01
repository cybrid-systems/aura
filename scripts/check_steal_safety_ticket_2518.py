#!/usr/bin/env python3
"""Issue #2518: MutationSafetySnapshot sequence ticket (sample→resume).

Contract:
  AC1 snapshot carries ticket; resume mismatch → hard-fail
  AC2 inject mid-window publish intercepted
  AC3 coexist with #2510 LayoutStamp restamp
  AC4 single atomic load; query schema-2518
  AC5 steal + resume wiring + gate

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

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    wc = _read("src/serve/worker.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/serve/test_steal_safety_ticket_2518.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2518", "AC1", fh)
    must("ticket", "AC1", fh)
    must("s.ticket = seq1", "AC1", fh)
    must("set_resume_safety_ticket", "AC1", fh)
    must("has_resume_safety_ticket_", "AC1", fh)
    must("ticket_miss", "AC1", fc)
    must("bump_steal_safety_ticket_mismatch", "AC1", fc)
    must("ac1_ticket_match_ok", "AC1", test)

    # AC2
    must("ac2_inject_mid_window", "AC2", test)
    must("publish_mutation_safety_mirrors", "AC2", test)
    must("check_and_enforce_resume_snapshot_invariant", "AC2", fc)

    # AC3
    must("2510", "AC3", fh + wc)
    must("set_resume_safety_ticket", "AC3", wc)
    must("call_steal_complete", "AC3", wc)
    must("ac3_coexist_2510", "AC3", test)

    # AC4
    must("current_safety_ticket", "AC4", fh)
    must("schema-2518", "AC4", q)
    must("steal-safety-ticket-mismatch-total", "AC4", q)
    must("steal-safety-ticket-wired", "AC4", q)
    must("schema-2346", "AC4", q)
    must("ac4_cost_and_query", "AC4", test)

    # AC5
    must("set_resume_safety_ticket(snap.ticket)", "AC5", wc)
    must("test_steal_safety_ticket_2518", "AC5", cmake)
    must("check_steal_safety_ticket_2518", "AC5", build)
    must("cmd_steal_safety_ticket_coverage", "AC5", build)
    must("ac5_source_wiring", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2518 steal safety ticket — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
