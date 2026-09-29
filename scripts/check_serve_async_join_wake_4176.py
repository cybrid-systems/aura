#!/usr/bin/env python3
"""Issue #4176 source-cite gate: serve-async spawn+join wake protocol + fd budget.

Soft --serve-async stalled the unix sock permanently after a long sequence of
sequential oneshot (fiber:join (fiber:spawn (lambda () (begin (mutate:rebind
...) (eval-current) ...)))) calls: the host timed out on the sock request, the
process stayed alive in ep_poll with near-zero CPU. Two root causes:

  1. Wrong-wake join protocol hole. The messaging fiber:join park is a wait
     protocol, not a one-shot wake. Spurious eventfd wakes are legal — the
     scheduler's stdin-event branch broadcasts a resume to EVERY Waiting fiber
     in wait_map_ (including fibers parked mid-join, whose state is Waiting
     via Fiber::yield(BlockingIO)), and serve_async's
     wake_waiting_session_fibers does the same for session fibers. A wrong
     wake made the join primitive treat the wake as target-done: it removed
     the joiner, observed a not-ready result slot, ERASED the pending slot
     and returned void. The orphaned child kept the denseness body mutex
     (s_cli_thread_fiber_body_mtx); the next spawn's body blocked on that
     mutex while running on the worker → the worker never ran again, the
     session fiber parked in join forever, no reply ever came (permanent
     stall, IO thread idle in ep_poll). Thresholds were load-dependent
     (~19-36 joins after helper denseness, ~64 in the original report),
     because the wrong wake needs to land while the previous child is
     mid-body — an intermittency window that grows with mutation churn
     (defuse_version climbing ~11 per join).

  2. fd leak. Scheduler::on_fiber_done EPOLL_CTL_DELs and erases the done
     fiber's wake eventfd but never closes the fd; completed fibers stay
     owned by the Scheduler (owned_fibers_, per #3905/#2468) until
     ~Scheduler, and ~Fiber (which closes the fd) only runs then. Every
     spawned fiber leaked one eventfd for the process lifetime: +1 fd per
     serve-async oneshot, so long Soft serves eventually hit RLIMIT_NOFILE
     (reproduced: fd table grew exactly +1 per oneshot; at the limit the
     Fiber ctor's eventfd(2) throws and, uncaught, kills the process).

Fix shape: (a) the join primitive loops — predicate re-checked after every
wake, spurious wakes re-register and re-park (mirroring Fiber::join), with a
reclaimed escape; (b) BlockingIO-parked fibers are excluded from both wake
broadcasts (their wake protocol is the target's on_fiber_done eventfd write);
(c) on_fiber_done closes the done fiber's eventfd (idempotent
Fiber::close_eventfd; ~Fiber keeps the backstop); (d) fiber:spawn degrades to
#f at scheduler failure (the #2656 contract) instead of unwinding through the
fiber trampoline; (e) session exec loops catch escaping exceptions and emit a
status error line so the session fiber survives (clear error, no silent
no-reply). The #4128 SIGTERM/SIG_DFL oneshot contract is untouched.

ACs:
  AC1  messaging.cpp fiber:join serve-async path loops until the target is
       done/reclaimed with a g_fiber_lookup re-resolution escape; cites
       #4176 (spurious wake = re-park, not result erase).
  AC2  scheduler.cpp stdin-event wait_map_ broadcast skips fibers parked
       with last_yield_reason() == YieldReason::BlockingIO; cites #4176.
  AC3  serve_async.cpp wake_waiting_session_fibers skips BlockingIO joiner
       parks with the same gate; cites #4176.
  AC4  scheduler.cpp on_fiber_done calls fiber->close_eventfd(); fiber.h
       declares `void close_eventfd() noexcept;`; fiber.cpp defines
       Fiber::close_eventfd (idempotent, stores -1); cites #4176.
  AC5  messaging.cpp fiber:spawn catches std::exception from the scheduler
       spawn bridge and returns #f (spawn_failed skips the thread-backend
       fallback); cites #4176 + keeps the #2656 positive-id contract.
  AC6  serve_async.cpp session exec loops (default + named) catch
       std::exception and emit a status error line ("exec exception: ") —
       the session fiber survives.
  AC7  tests cite #4176 (runtime ACs in tests/serve/test_concurrent.cpp:
       join wake protocol, 256-oneshot fd budget, source-cite); no
       tests/serve/test_issue_4176.cpp; no docs/design/4176-*; build.py
       wires this linter; scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MESSAGING = ROOT / "src" / "compiler" / "evaluator_primitives_messaging.cpp"
SCHED = ROOT / "src" / "serve" / "scheduler.cpp"
SERVE = ROOT / "src" / "serve" / "serve_async.cpp"
FIBER_H = ROOT / "src" / "serve" / "fiber.h"
FIBER_CPP = ROOT / "src" / "serve" / "fiber.cpp"
TST = ROOT / "tests" / "serve" / "test_concurrent.cpp"


def _join_loop_region(text: str) -> str:
    """fiber:join primitive region around the #4176 wait loop."""
    idx = text.find("Issue #4176: the BlockingIO park is a *wait")
    if idx < 0:
        return ""
    end = text.find("// ── orch:metrics", idx)
    return text[idx:end] if end > 0 else text[idx : idx + 6000]


def _spawn_region(text: str) -> str:
    """fiber:spawn primitive region (add("fiber:spawn", ...) onward)."""
    idx = text.find('add("fiber:spawn"')
    if idx < 0:
        return ""
    end = text.find('add("fiber:spawn-backend"', idx)
    return text[idx:end] if end > 0 else text[idx : idx + 12000]


def _on_fiber_done_region(text: str) -> str:
    idx = text.find("void Scheduler::on_fiber_done")
    if idx < 0:
        return ""
    end = text.find("// Issue #119: add a joiner fiber", idx)
    return text[idx:end] if end > 0 else text[idx : idx + 6000]


def main() -> int:
    msg = MESSAGING.read_text() if MESSAGING.exists() else ""
    sched = SCHED.read_text() if SCHED.exists() else ""
    serve = SERVE.read_text() if SERVE.exists() else ""
    fiber_h = FIBER_H.read_text() if FIBER_H.exists() else ""
    fiber_cpp = FIBER_CPP.read_text() if FIBER_CPP.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg_out: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg_out}")
        ok = ok and good

    join_loop = _join_loop_region(msg)
    spawn_region = _spawn_region(msg)
    on_done = _on_fiber_done_region(sched)

    # ── AC1: join wait loop ──
    report(
        "AC1 join-wake-loop",
        "Issue #4176: the BlockingIO park is a *wait" in join_loop
        and "while (true)" in join_loop
        and "target->is_done() || target->is_reclaimed()" in join_loop
        and "aura::messaging::g_fiber_lookup(fid)" in join_loop
        and "remove_joiner" in join_loop,
        "fiber:join loops on spurious wakes with reclaimed + unregistered escapes (#4176)",
    )

    # ── AC2: scheduler stdin broadcast gate ──
    report(
        "AC2 stdin-broadcast-gate",
        "Issue #4176: never stdin-broadcast a fiber" in sched
        and "fiber->last_yield_reason() == YieldReason::BlockingIO" in sched,
        "scheduler stdin-event broadcast skips BlockingIO joiner parks (#4176)",
    )

    # ── AC3: session-fiber enqueue gate ──
    report(
        "AC3 session-wake-gate",
        "Issue #4176: skip fibers parked in fiber:join" in serve
        and serve.count("f->last_yield_reason() == YieldReason::BlockingIO") >= 1,
        "wake_waiting_session_fibers skips BlockingIO joiner parks (#4176)",
    )

    # ── AC4: fd budget close ──
    report(
        "AC4 eventfd-close",
        "fiber->close_eventfd();" in on_done
        and "Issue #4176" in on_done
        and "void close_eventfd() noexcept;" in fiber_h
        and "void Fiber::close_eventfd() noexcept" in fiber_cpp
        and "eventfd_ = -1;" in fiber_cpp,
        "on_fiber_done closes the done fiber's wake eventfd; idempotent Fiber::close_eventfd (#4176)",
    )

    # ── AC5: spawn degrade (#2656 contract preserved) ──
    report(
        "AC5 spawn-degrade",
        "bool spawn_failed = false;" in spawn_region
        and "catch (const std::exception&)" in spawn_region
        and "return make_bool(false);" in spawn_region
        and "0x4000" in spawn_region,
        "fiber:spawn catches scheduler-bridge failure → #f, thread fallback skipped, #2656 ids intact (#4176)",
    )

    # ── AC6: session exec catch (default + named loops) ──
    report(
        "AC6 exec-catch",
        serve.count("exec exception: ") >= 2 and serve.count("catch (const std::exception& e)") >= 2,
        "both session exec loops catch escaping exceptions → status error, session survives (#4176)",
    )

    # ── AC7: tests + wiring ──
    no_issue_test_file = not (ROOT / "tests" / "serve" / "test_issue_4176.cpp").exists()
    no_design_doc = not any((ROOT / "docs" / "design").glob("4176-*"))
    report(
        "AC7 tests-wiring",
        "#4176 AC1: join wake protocol" in tst
        and "#4176 AC2: fd budget across 256 sequential spawn oneshots" in tst
        and "#4176 AC3: source-cite" in tst
        and "test_issue_4176_join_wake_protocol" in tst
        and "test_issue_4176_fd_budget_oneshots" in tst
        and no_issue_test_file
        and no_design_doc
        and "check_serve_async_join_wake_4176.py" in build
        and "check_serve_async_join_wake_4176.py" in allow,
        "runtime ACs in test_concurrent.cpp; no test_issue_4176.cpp; no docs/design/4176-*; build.py + allowlist wired",
    )

    print()
    if not ok:
        print("check_serve_async_join_wake_4176: FAIL")
        return 1
    print("check_serve_async_join_wake_4176: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
