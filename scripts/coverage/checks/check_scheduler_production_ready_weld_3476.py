#!/usr/bin/env python3
"""Issue #3476: Scheduler::run welds production Ready before WorkerThread::start.

#3098 Ready exists; nothing in the serve start path called it, so
g_production_multi_worker_latched stayed 0 and I3/I6 arms were no-ops.
Weld: workers_.size()>1 → aura_runtime_require_production_multi_worker
before w->start(); size==1 → aura_runtime_require_production_abi.
No new counters / query keys / steal protocol.

Contract:
  AC1  workers_.size()>1 calls multi-worker Ready before w->start()
  AC2  successful Ready latches (store in Ready; Scheduler calls it)
  AC3  size==1 uses require_production_abi
  AC4  no new query key / g_3476_*
  AC5  extend test_steal_complete_strong_entry; no invent
  AC6  source-cite Scheduler::run; linter AFTER #3195

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    sched = _read("src/serve/scheduler.cpp")
    abi = _read("src/serve/runtime_production_abi.cpp")
    test = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    build = _read("build.py")

    run = sched.find("void Scheduler::run()")
    if run < 0:
        fails.append("AC6: Scheduler::run missing")
        win = ""
    else:
        start = sched.find("w->start()", run)
        win = sched[run:start] if start > run else sched[run : run + 2500]
        if start < 0:
            fails.append("AC1: w->start() missing")
        must("Issue #3476", "AC6 cite", win)
        must("workers_.size() > 1", "AC1 size gate", win)
        must("aura_runtime_require_production_multi_worker", "AC1 Ready", win)
        must("aura_runtime_require_production_abi()", "AC3 single-worker ABI", win)
        if start > run:
            if sched.find("aura_runtime_require_production_multi_worker", run) > start:
                fails.append("AC1: multi-worker Ready must dominate w->start()")
            if sched.find("aura_runtime_require_production_abi()", run) > start:
                fails.append("AC3: single-worker ABI must dominate w->start()")

    must("g_production_multi_worker_latched.store(1", "AC2 latch in Ready", abi)
    must("aura_runtime_require_production_multi_worker", "AC1 Ready def", abi)

    must("3476 AC1: multi-worker Ready before WorkerThread::start", "AC5 AC1", test)
    must("3476 AC2: Ready latched after Scheduler::run", "AC5 AC2", test)
    must("3476 AC3: single-worker uses require_production_abi before start", "AC5 AC3", test)
    must_not("schema-3476", "AC4 no query key", sched + abi)
    must_not("g_3476_", "AC4 no g_3476_*", sched)

    must("check_scheduler_production_ready_weld_3476", "AC6 build.py", build)
    prev = build.find("check_production_multi_worker_residual_sticky_3195")
    ours = build.find("check_scheduler_production_ready_weld_3476")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3195")

    if (ROOT / "tests" / "serve" / "test_issue_3476.cpp").is_file():
        fails.append("AC5: test_issue_3476.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3476-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3476 scheduler_production_ready_weld:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3476 scheduler_production_ready_weld: Ready before start; Soft single-worker ABI")
    return 0


if __name__ == "__main__":
    sys.exit(main())
