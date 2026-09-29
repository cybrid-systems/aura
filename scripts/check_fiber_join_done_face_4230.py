#!/usr/bin/env python3
"""Issue #4230 source-cite gate: fiber join register-after-notify lost wake (sequential spawn+join serve timeout).

Measured Soft --serve-async sequential (fiber:join (fiber:spawn (lambda ()
7))) oneshots: 16/32/48/64 all ok (~1.5s each on Soft tip e34f879), then a
96-batch failed at i=7 with the client's 8s serve_session_timeout after a
successful 64-batch on the SAME session. Not a Ready/multi-worker claim —
denseness stamp path (fiber_fanout_probe) only does one spawn+join; the
real stress is sequential.

Root cause — register-after-notify lost wake (residual window after the
#4176 wrong-wake fix). The messaging fiber:join caller (and Fiber::join)
probes target->is_done() BEFORE registering on joiner_map_. If the target
completes in that gap, Scheduler::on_fiber_done runs its joiner-notify
pass with the joiner STILL ABSENT from joiner_map_: the
write-1-to-joiner-eventfd wake never fires. add_joiner then finds the done
fiber via fiber_by_id (done fibers stay owned until ~Scheduler), registers
the joiner, and the joiner parks in BlockingIO FOREVER — nobody will ever
write its eventfd. Under sustained sequential spawn+join load the session
fiber stops replying and the serve-async sock stalls past the client's
raw_line budget (measured: 64 oneshots ok, then a serve_session_timeout
mid-batch; #4176's AC2 fd-budget door did not cover this because a parked
joiner leaks no fd — the fiber just never wakes).

Fix shape — done-face re-check under the notify mutex.
Scheduler::add_joiner re-checks target->is_done() INSIDE the
joiner_map_mutex_ critical section, BEFORE registering (with the #4230
rationale comment). on_fiber_done's notify pass takes the same mutex, so
the two critical sections are mutually exclusive:

  - Notify-pass-before-us: the worker's state_.store(Done, release) is
    sequenced-before notify_fiber_done, and the mutex chain publishes it —
    the re-check observes Done and returns false. The caller treats false
    as already-done/vanished: Fiber::join re-checks is_done() and returns
    JoinStatus::Ok; the messaging fiber:join loop breaks to the result
    fetch. No park, result is ready.
  - Us-before-notify: our entry is present when the pass runs; the
    joiner-eventfd write fires and the parked joiner wakes.

No interleaving can leave a registered joiner unwoken.

ACs:
  AC1  scheduler.cpp add_joiner re-checks target->is_done() AFTER the
       joiner_map_mutex_ acquire and BEFORE the registration, citing #4230
       (register-after-notify lost-wake closure); returns false on Done.
  AC2  Callers honor the refusal as already-done: Fiber::join (fiber.cpp)
       re-checks is_done() and finishes JoinStatus::Ok on add_joiner false;
       the messaging fiber:join loop breaks to its result fetch on
       add_joiner false.
  AC3  tests/serve/test_concurrent.cpp carries the #4230 runtime doors
       (done-face door + 200 sequential spawn+join oneshots + source-cite);
       no tests/serve/test_issue_4230.cpp; no docs/design/4230-*; build.py
       wires this linter; scripts/coverage/root_check_allowlist.txt lists
       it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHED = ROOT / "src" / "serve" / "scheduler.cpp"
FIBER_CPP = ROOT / "src" / "serve" / "fiber.cpp"
MESSAGING = ROOT / "src" / "compiler" / "evaluator_primitives_messaging.cpp"
TST = ROOT / "tests" / "serve" / "test_concurrent.cpp"


def _add_joiner_region(text: str) -> str:
    idx = text.find("bool Scheduler::add_joiner")
    if idx < 0:
        return ""
    end = text.find("// Issue #119: remove a joiner", idx)
    return text[idx:end] if end > 0 else text[idx : idx + 4000]


def main() -> int:
    sched = SCHED.read_text() if SCHED.exists() else ""
    fiber = FIBER_CPP.read_text() if FIBER_CPP.exists() else ""
    messaging = MESSAGING.read_text() if MESSAGING.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg_out: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg_out}")
        ok = ok and good

    region = _add_joiner_region(sched)

    # ── AC1: done-face re-check under joiner_map_mutex_ ──
    lock_pos = region.find("AuditedMutexLock")
    done_pos = region.find("if (target->is_done())")
    report(
        "AC1 add-joiner-done-face",
        "Issue #4230: register-after-notify lost-wake closure" in region
        and lock_pos != -1
        and done_pos != -1
        and done_pos > lock_pos
        and "return false;" in region,
        "add_joiner re-checks the done face under joiner_map_mutex_ before "
        "registering and returns false on Done (#4230)",
    )

    # ── AC2: callers honor the refusal as already-done ──
    report(
        "AC2 caller-already-done",
        "if (!g_scheduler->add_joiner(target->id(), g_current_fiber))" in fiber
        and "if (target->is_done())" in fiber
        and "g_scheduler->add_joiner(static_cast<std::uint64_t>(fid)" in messaging,
        "Fiber::join re-checks is_done() → JoinStatus::Ok on refusal; the messaging "
        "fiber:join loop breaks to the result fetch (#4230)",
    )

    # ── AC3: runtime doors + wiring ──
    no_issue_file = not (ROOT / "tests" / "serve" / "test_issue_4230.cpp").exists()
    no_docs = not any((ROOT / "docs" / "design").glob("4230-*")) if (ROOT / "docs" / "design").exists() else True
    report(
        "AC3 test-door-and-wiring",
        "#4230" in tst
        and "test_issue_4230_add_joiner_done_face" in tst
        and "test_issue_4230_sequential_join_oneshots" in tst
        and "test_issue_4230_source_cite" in tst
        and "check_fiber_join_done_face_4230.py" in build
        and "check_fiber_join_done_face_4230.py" in allow
        and no_issue_file
        and no_docs,
        "tests/serve/test_concurrent.cpp carries the #4230 doors (done-face door, "
        "200 sequential spawn+join oneshots, source-cite); build.py wires this "
        "linter; allowlist lists it; no test_issue_4230.cpp; no docs/design/4230-*",
    )

    if not ok:
        print("FAIL: check_fiber_join_done_face_4230")
        return 1
    print("PASS: check_fiber_join_done_face_4230 (3 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
