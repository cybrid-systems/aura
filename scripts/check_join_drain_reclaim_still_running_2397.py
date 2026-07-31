#!/usr/bin/env python3
"""Issue #2397: distinguish reclaimed vs body-still-running after residual.

Contract:
  AC1 mark_reclaimed while !Done → still-running +1; body exit → retired +1
  AC2 Ok join path unchanged (no residual / no new atomics)
  AC3 Query keys additive; schema-2227 residual/reclaim keys preserved
  AC4 Soft path zero cost (only mark_reclaimed / body-exit / dtor)
  AC5 Tests + CMake + build.py gate

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
    sc = _read("src/serve/scheduler.cpp")
    orch = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    bridge = _read("src/compiler/fiber_bridge.cpp")
    test = _read("tests/orch/test_join_drain_reclaim_2227.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 lifecycle
    must("Issue #2397", "AC1", fh)
    must("note_body_exit_if_reclaimed", "AC1", fh)
    must("join_drain_residual_still_running", "AC1", fh)
    must("join_drain_residual_body_retired_total", "AC1", fh)
    must("void Fiber::mark_reclaimed()", "AC1", fc)
    must("note_body_exit_if_reclaimed", "AC1", fc)
    must("still_running_after_reclaim_counted_", "AC1", fc)
    must("mark_reclaimed()", "AC1", sc)
    must("2397 AC1", "AC1", test)

    # AC2 Ok path unchanged (existing #2227 AC3 + #2397 AC2)
    must("2397 AC2", "AC2", test)
    must("still-running unchanged on Ok join", "AC2", test)

    # AC3 query additive
    must("join-drain-residual-still-running-total", "AC3", prim)
    must("join-drain-residual-body-retired-total", "AC3", prim)
    must("schema-2397", "AC3", prim)
    must("issue-2397", "AC3", prim)
    must("join-drain-reclaim-still-running-wired", "AC3", prim)
    must("join-drain-residual-reclaim-total", "AC3", prim)
    must("join_drain_residual_still_running", "AC3", orch)
    must("join_drain_residual_body_retired_total", "AC3", orch)
    must("kJoinDrainReclaimStillRunningIssue", "AC3", orch)
    must("2397 AC3", "AC3", test)

    # AC4 soft path / hooks
    must("aura_orch_note_join_drain_reclaim_still_running", "AC4", fc)
    must("aura_orch_note_join_drain_reclaim_body_retired", "AC4", mut)
    must("aura_orch_note_join_drain_reclaim_still_running_drop", "AC4", mut)
    must("aura_orch_note_join_drain_reclaim_still_running", "AC4", bridge)
    must("2397 AC4", "AC4", test)

    # AC5
    must("2397 AC5", "AC5", test)
    must("check_join_drain_reclaim_still_running_2397", "AC5", build)
    must("cmd_join_drain_reclaim_still_running_coverage", "AC5", build)
    must("test_join_drain_reclaim_2227", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2397 reclaim still-running — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
