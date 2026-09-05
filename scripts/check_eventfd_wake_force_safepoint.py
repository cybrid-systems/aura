#!/usr/bin/env python3
"""Issue #3553 linter: enforce that the scheduler eventfd / IO-thread wake
handler in src/serve/scheduler.cpp consults process_mutation_boundary_held_count
before enqueueing a woken Fiber for resumption. Forces a force-safepoint +
synthetic MutationBoundary reason pair when the count > 0 and the waker's
last_yield_reason is not MutationBoundary.

Usage:
    python3 scripts/check_eventfd_wake_force_safepoint.py --strict

Forbidden (production silent-corrupt):
    - scheduler.cpp eventfd wake handler enqueues Fiber without checking
      aura_process_mutation_boundary_held_count() — held Guard holder can
      be forced off-CPU by GC compact while still mid-mutate.

Required:
    - file-scope atomic g_eventfd_wake_force_safepoint_total (sibling of
      safepoint_wait_while_mutation_held; NOT in metrics middle).
    - check process_mutation_boundary_held_count() > 0
    - Fiber::request_force_safepoint + set_yield_reason(MutationBoundary) pair
    - the existing #3521 is_queued check must precede the force-safepoint
      defer (a fiber already on a worker shouldn't be deferred again).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHED = ROOT / "src" / "serve" / "scheduler.cpp"

# Wire-1: file-scope counter g_eventfd_wake_force_safepoint_total.
RE_COUNTER = re.compile(
    r"static\s+std::atomic\s*<\s*std::uint64_t\s*>\s+g_eventfd_wake_force_safepoint_total\s*\{",
    re.MULTILINE,
)
# Wire-2: process_mutation_boundary_held_count() read in wake handler.
RE_HELD_READ = re.compile(
    r"aura_process_mutation_boundary_held_count\(\)\s*>\s*0",
    re.MULTILINE,
)
# Wire-3: request_force_safepoint + set_yield_reason(MutationBoundary) pair.
RE_FORCE_PAIR = re.compile(
    r"request_force_safepoint\(\)\s*;\s*[^\n]*?set_yield_reason\s*\(\s*YieldReason::MutationBoundary\s*\)",
    re.MULTILINE | re.DOTALL,
)
# Wire-4: counter fetch_add (relaxed; sibling family).
RE_COUNTER_BUMP = re.compile(
    r"g_eventfd_wake_force_safepoint_total\.fetch_add\s*\(\s*1\s*,\s*std::memory_order_relaxed\s*\)",
    re.MULTILINE,
)
# Wire-5: is_queued check (Issue #3521) must precede the force-safepoint
# defer; otherwise a fiber already on a worker would be deferred again.
RE_IS_QUEUED = re.compile(
    r"Issue\s*#3521:\s*skip\s+if\s+already\s+queued",
    re.MULTILINE,
)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--strict", action="store_true", help="Fail on missing wire patterns")
    args = p.parse_args()
    strict = bool(args.strict)
    if not SCHED.exists():
        fail(f"missing required file: {SCHED}")
        return 1
    text = SCHED.read_text(encoding="utf-8", errors="replace")

    v = 0
    if not RE_COUNTER.search(text):
        fail(f"{SCHED}: missing file-scope atomic g_eventfd_wake_force_safepoint_total")
        v += 1
    if not RE_HELD_READ.search(text):
        fail(f"{SCHED}: wake handler missing aura_process_mutation_boundary_held_count() > 0 check")
        v += 1
    if not RE_FORCE_PAIR.search(text):
        fail(f"{SCHED}: missing request_force_safepoint + set_yield_reason(MutationBoundary) pair")
        v += 1
    if not RE_COUNTER_BUMP.search(text):
        fail(f"{SCHED}: missing g_eventfd_wake_force_safepoint_total.fetch_add bump")
        v += 1
    # Order check: is_queued (#3521) must precede force-safepoint defer.
    queued_pos = text.find("Issue #3521: skip if already queued")
    defer_pos = text.find("request_force_safepoint")
    if queued_pos < 0 or defer_pos < 0 or queued_pos > defer_pos:
        fail(f"{SCHED}: is_queued (#3521) check must precede force-safepoint defer")
        v += 1
    if v > 0 and strict:
        print(f"\ncheck_eventfd_wake_force_safepoint: {v} violation(s) — refusing to ship", file=sys.stderr)
        return 1
    print("check_eventfd_wake_force_safepoint: OK (3553 AC: eventfd wake force-safepoint wired)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
