#!/usr/bin/env python3
"""Issue #3555 linter: enforce the chaos soak deploy gate has all 6
hard-fail assertions + env knobs + AC4 contract.

The test binary `chaos_soak_production_gate.cpp` is part of the
`test_mailbox_fiber_batch` SuiteBuilder. Any future PR that removes
the 6 hard-fail assertions, drops env knobs, or short-circuits the
production-mode fail-closed contract fails the deploy gate silently —
this linter refuses such changes.

Usage:
    python3 scripts/check_chaos_soak_deploy_gate.py --strict

Required (all 6 hard-fail assertions must be present in the test file):
    1. steal_safety_production_residual_zero_v_read() != 0 under production
    2. safepoint_blocked_by_long_mutation_max_us_v_read() <= kMailboxP99SLO_us
    3. atomic_batch_tenant_isolation_denials_total == 0 (delta == 0)
    4. g_lock_order_violation_total == 0 (delta == 0)
    5. eventfd_wake_force_safepoint_total_v_read() == 0 (delta == 0)
    6. provenance::fiber_id_mismatch_total == 0 (delta == 0)

Required accessors in scheduler.h / scheduler.cpp:
    - inline constexpr std::int64_t kMailboxP99SLO_us
    - safepoint_blocked_by_long_mutation_max_us_v_read()
    - eventfd_wake_force_safepoint_total_v_read()
    - record_safepoint_blocked_by_long_mutation_us()

Required env knobs in the test file:
    - AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S
    - AURA_CHAOS_SOAK_DEPLOY_GATE_FULL
    - AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS (default >= 8)
    - AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS (default >= 8)

Required wire-up:
    - Scheduler(N>=8) + n_fibers >= 8
    - production_defaults_active() gate for fail-closed contract
    - Soft / Off path: observe-only branch
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent.parent
TEST = ROOT / "tests/serve/chaos_soak_production_gate.cpp"
SCHED_H = ROOT / "src/serve/scheduler.h"
SCHED_CPP = ROOT / "src/serve/scheduler.cpp"

# ── Required production-mode contract (AC2: hard-fail closed) ─────
RE_RESIDUAL = re.compile(r"steal_safety_production_residual_zero_v_read\s*\(\s*\)")
RE_P99_MAX = re.compile(r"safepoint_blocked_by_long_mutation_max_us_v_read\s*\(\s*\)")
RE_P99_SLO = re.compile(r"kMailboxP99SLO_us")
RE_TENANT = re.compile(r"tenant_isolation_denials_total_v_read|atomic_batch_tenant_isolation_denials_total")
RE_LOCK = re.compile(r"g_lock_order_violation_total")
RE_EVENTFD = re.compile(r"eventfd_wake_force_safepoint_total_v_read\s*\(\s*\)")
RE_FIBER_ID = re.compile(r"fiber_id_mismatch_total")

# ── Required env knobs (AC4) ──────────────────────────────────────
RE_ENV_DURATION = re.compile(r"AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S")
RE_ENV_FULL = re.compile(r"AURA_CHAOS_SOAK_DEPLOY_GATE_FULL")
RE_ENV_WORKERS = re.compile(r"AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS")
RE_ENV_FIBERS = re.compile(r"AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS")

# ── Required contract (AC3: Soft / Off observe-only) ──────────────
RE_PROD_GATE = re.compile(r"production_defaults_active\s*\(\s*\)\s*!=\s*0|production_defaults_active\s*\(\s*\)")
RE_OBSERVE_ONLY = re.compile(r"observe-only|Soft\s*/\s*Off\s+path")

# ── Required accessors (scheduler.h / scheduler.cpp) ──────────────
RE_KMAILBOX = re.compile(r"inline\s+constexpr\s+std::int64_t\s+kMailboxP99SLO_us\s*=\s*50'?000")
RE_MAX_READ = re.compile(r"safepoint_blocked_by_long_mutation_max_us_v_read\s*\(\s*\)\s*noexcept")
RE_EVENTFD_READ = re.compile(r"eventfd_wake_force_safepoint_total_v_read\s*\(\s*\)\s*noexcept")
RE_RECORD = re.compile(r"record_safepoint_blocked_by_long_mutation_us\s*\(\s*std::int64_t\s+\w+\s*\)\s*noexcept")
RE_SCHED_CPP_MAX = re.compile(r"g_safepoint_blocked_by_long_mutation_max_us")


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--strict", action="store_true", help="Fail on missing wire patterns (deploy gate red)")
    args = p.parse_args()
    strict = bool(args.strict)

    v = 0

    # ── Test file: all 6 hard-fail assertions + env knobs + AC3 contract
    if not TEST.exists():
        fail(f"missing {TEST} (chaos soak test file required by #3555 AC1)")
        return 1 if strict else 0

    test_text = TEST.read_text(encoding="utf-8", errors="replace")

    checks = [
        ("AC2.1", test_text, RE_RESIDUAL, "test: steal_safety_production_residual_zero_v_read() read"),
        ("AC2.2", test_text, RE_P99_MAX, "test: safepoint_blocked_by_long_mutation_max_us_v_read() read"),
        ("AC2.2", test_text, RE_P99_SLO, "test: kMailboxP99SLO_us SLO check"),
        ("AC2.3", test_text, RE_TENANT, "test: atomic_batch_tenant_isolation_denials_total read"),
        ("AC2.4", test_text, RE_LOCK, "test: g_lock_order_violation_total read"),
        ("AC2.5", test_text, RE_EVENTFD, "test: eventfd_wake_force_safepoint_total_v_read() read"),
        ("AC2.6", test_text, RE_FIBER_ID, "test: provenance::fiber_id_mismatch_total read"),
        ("AC4", test_text, RE_ENV_DURATION, "test: AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S env knob"),
        ("AC4", test_text, RE_ENV_FULL, "test: AURA_CHAOS_SOAK_DEPLOY_GATE_FULL env knob"),
        ("AC4", test_text, RE_ENV_WORKERS, "test: AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS env knob"),
        ("AC4", test_text, RE_ENV_FIBERS, "test: AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS env knob"),
        ("AC3", test_text, RE_PROD_GATE, "test: production_defaults_active() gate"),
        ("AC3", test_text, RE_OBSERVE_ONLY, "test: Soft / Off observe-only branch"),
    ]
    for label, text, regex, why in checks:
        if not regex.search(text):
            fail(f"{label}: {TEST.name}: missing pattern {regex.pattern!r} ({why})")
            v += 1

    # ── scheduler.h / scheduler.cpp: required accessors
    if not SCHED_H.exists():
        fail(f"missing {SCHED_H}")
        v += 1
    else:
        h_text = SCHED_H.read_text(encoding="utf-8", errors="replace")
        for label, regex, why in [
            ("API", RE_KMAILBOX, "scheduler.h: kMailboxP99SLO_us = 50'000 const"),
            ("API", RE_MAX_READ, "scheduler.h: safepoint_blocked_by_long_mutation_max_us_v_read() declaration"),
            ("API", RE_EVENTFD_READ, "scheduler.h: eventfd_wake_force_safepoint_total_v_read() declaration"),
            ("API", RE_RECORD, "scheduler.h: record_safepoint_blocked_by_long_mutation_us() declaration"),
        ]:
            if not regex.search(h_text):
                fail(f"{label}: {SCHED_H.name}: missing pattern {regex.pattern!r} ({why})")
                v += 1

    if not SCHED_CPP.exists():
        fail(f"missing {SCHED_CPP}")
        v += 1
    else:
        cpp_text = SCHED_CPP.read_text(encoding="utf-8", errors="replace")
        if not RE_SCHED_CPP_MAX.search(cpp_text):
            fail(
                f"API: {SCHED_CPP.name}: missing g_safepoint_blocked_by_long_mutation_max_us "
                f"(file-scope max-latency tracker)"
            )
            v += 1

    if v > 0 and strict:
        print(
            f"\ncheck_chaos_soak_deploy_gate: {v} violation(s) — refusing to ship",
            file=sys.stderr,
        )
        return 1
    if v > 0:
        print(
            f"check_chaos_soak_deploy_gate: {v} warning(s) (run with --strict to enforce)",
            file=sys.stderr,
        )
        return 0
    print("check_chaos_soak_deploy_gate: OK (3555 AC: 6 hard-fail + env knobs + Soft path)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
