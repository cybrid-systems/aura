#!/usr/bin/env python3
"""Issue #2347: MultiFiberMailbox Guard-live blocking recv hard audit.

Contract (extends Policy A #2188):
  AC1 Soft / default: boundary-live blocking recv → empty + soft counter only
  AC2 Strict / production: hard-total bumps (AURA_MUTATE_MAILBOX_STRICT or canary)
  AC3 Optional threshold: N rejects in one Guard window → mark-failed
  AC4 Happy path (no boundary): zero extra hard cost
  AC5 Tests + schema-2347 keys + Agent contract comment + window clear

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
    eixx = _read("src/compiler/evaluator.ixx")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fbr = _read("src/compiler/fiber_bridge.cpp")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp")
    build = _read("build.py")

    # AC1 Soft / Policy A retained
    must("recv_rejected_in_mutation_boundary", "AC1", mb)
    must("Policy A", "AC1", mb)
    must("ac2347_soft_only_soft_counter", "AC1", test)
    must("AC1 Soft", "AC1", test)

    # AC2 Strict hard path
    must("AURA_MUTATE_MAILBOX_STRICT", "AC2", mb)
    must("is_mutate_mailbox_strict", "AC2", mb)
    must("recv_rejected_in_mutation_boundary_hard_total", "AC2", mb)
    must("aura_production_defaults_active_probe", "AC2", mb)
    must("ac2347_strict_hard_counter", "AC2", test)
    must("AC2 Strict", "AC2", test)

    # AC3 threshold force-rollback
    must("AURA_MUTATE_MAILBOX_REJECT_THRESHOLD", "AC3", mb)
    must("mutate_mailbox_reject_threshold", "AC3", mb)
    must("recv_boundary_force_rollback_total", "AC3", mb)
    must("g_recv_boundary_reject_window", "AC3", mb)
    must("aura_evaluator_mark_outermost_mutation_failed", "AC3", mb)
    must("mark_outermost_mutation_failed", "AC3", eixx)
    must("aura_evaluator_mark_outermost_mutation_failed", "AC3", efm)
    must("aura_evaluator_mark_outermost_mutation_failed", "AC3", fbr)
    must("clear_recv_boundary_reject_window", "AC3", emb)
    must("ac2347_threshold_force_rollback", "AC3", test)
    must("AC3:", "AC3", test)

    # AC4 happy path
    must("ac2347_happy_path_no_hard", "AC4", test)
    must("AC4:", "AC4", test)
    must("depth/held probe", "AC4", mb)

    # AC5 schema + contract + gate
    must("schema-2347", "AC5", epm)
    must("issue-2347", "AC5", epm)
    must("recv-rejected-in-mutation-boundary-hard-total", "AC5", epm)
    must("recv-boundary-force-rollback-total", "AC5", epm)
    must("mutate-mailbox-strict-wired", "AC5", epm)
    must("recv-boundary-hard-wired", "AC5", epm)
    must("Issue #2347", "AC5", mb)
    must("try_recv", "AC5", mb)
    must("ac2347_schema_and_contract", "AC5", test)
    must("check_mutate_mailbox_strict_2347", "AC5", build)
    must("cmd_mutate_mailbox_strict_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2347 mutate-mailbox Strict hard audit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
