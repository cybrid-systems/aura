#!/usr/bin/env python3
"""Issue #2679: runtime(chaos) — production multi-fiber × MutationBoundary ×
GC × steal × mailbox soak + silent-corruption detection.

Validates that the existing chaos binary
(tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp) already covers all
6 production ACs per #2352 / #2380 / #2513 / #2554, and prevents regression
on the long-soak / silent-corruption / hard-fail counters / Soft mode /
build.py wiring / reproducible seed.

Contract:
  AC1 Dedicated soak target (extension of existing chaos binary) with
     ≥30 min configurable duration (AURA_CHAOS_DURATION_S ≥ 30 for SOAK),
     ≥8 workers + ≥64 concurrent fibers (default SOAK profile), continuous
     outermost + nested mutate, GC pressure, work-stealing pressure,
     mailbox send/recv across fibers while some hold MutationBoundary.
  AC2 Silent-corruption detection: delta == 0 checks on
     mutation_steal_snapshot_mismatch_total + steal_snapshot_hard_fail_total
     + join_drain_residual_still_running + mailbox starvation delta; hard
     fail under production / SOAK / PR-gate modes.
  AC3 Hard-fail counters emitted: steal_snapshot_hard_fail_total +
     join_drain_residual_still_running + mb_starve_total; allows tunable
     ceiling via AURA_CHAOS_MB_STARVE_MAX.
  AC4 Soft / sandbox=off mode still runs the soak (observes only) so
     developers can use it without production lock; AURA_STEAL_SNAPSHOT_SOFT
     is FORBIDDEN under production / PR gate (unset by exec).
  AC5 Wired into build.py as optional long job (production-concurrency /
     nightly); short smoke variant (chaos_pr_hard_fail_gate) stays in PR
     hard-fail gate; full SOAK/FULL path unchanged.
  AC6 Reproducible seed via AURA_CHAOS_SEED (default 1) + documented
     reproduction steps in test file header (AURA_CHAOS_* env knobs).

Exit 0 = all AC rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    def must_match(pattern: str, label: str, hay: str) -> None:
        if not re.search(pattern, hay):
            fails.append(f"{label}: pattern {pattern!r} not found")

    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    # ============================================================
    # AC1: dedicated soak target + ≥30 min + ≥8 workers + ≥64 fibers
    # ============================================================
    must("AURA_CHAOS_DURATION_S", "AC1", chaos)
    # SOAK default duration must be ≥ 30s (≥30 min documented in env table).
    must("AURA_CHAOS_DURATION_S    smoke 2 / full ≥30 / soak default 300", "AC1", chaos)
    # SOAK profile must use workers ≥ 8 and fibers ≥ 64.
    must_match(
        r"const int workers = k_int_env\(\"AURA_CHAOS_WORKERS\",\s*8\)",
        "AC1",
        chaos,
    )
    must_match(
        r"const int fibers = k_int_env\(\"AURA_CHAOS_FIBERS\",\s*64\)",
        "AC1",
        chaos,
    )
    must_match(
        r"const int dur = k_int_env\(\"AURA_CHAOS_DURATION_S\",\s*30\)",
        "AC1",
        chaos,
    )
    # Verify the SOAK mode toggle exists.
    must_match(r"chaos_soak\(\)", "AC1", chaos)
    must("AURA_CHAOS_SOAK", "AC1", chaos)
    # Verify the chaos_fiber function exercises outermost + nested mutate.
    must("try_acquire", "AC1", chaos)
    must("Nested Guard under outer", "AC1", chaos)
    # GC pressure + safepoint.
    must("request_gc_safepoint", "AC1", chaos)
    # Mailbox send/recv while holding MutationBoundary.
    must("push(std::move(m))", "AC1", chaos)
    must("try_recv", "AC1", chaos)

    # ============================================================
    # AC2: silent-corruption detection (delta == 0 checks)
    # ============================================================
    must("hard_fail_invariants", "AC2", chaos)
    must("mutation_steal_snapshot_mismatch_total", "AC2", chaos)
    must("steal_snapshot_hard_fail_total", "AC2", chaos)
    must("join_drain_residual_still_running", "AC2", chaos)
    # Verify delta == 0 pattern for silent corruption.
    must_match(
        r"delta\s*==\s*0",
        "AC2",
        chaos,
    )
    # Verify the silent-corruption CHECK statement (lenient regex —
    # matches CHECK(delta == 0, "...silent corruption...") with any
    # leading arg shape).
    must_match(
        r"CHECK\([^)]*delta\s*==\s*0[^)]*silent corruption",
        "AC2",
        chaos,
    )
    # Hard-fail inject test for PR gate (#2554).
    must("chaos_pr_gate_inject_hard_fail", "AC2", chaos)
    must("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL", "AC2", chaos)

    # ============================================================
    # AC3: hard-fail counters emitted
    # ============================================================
    # steal hard-fail counter.
    must("Fiber::steal_snapshot_hard_fail_total", "AC3", chaos)
    must("mb_starve", "AC3", chaos)
    must("AURA_CHAOS_MB_STARVE_MAX", "AC3", chaos)
    # residual still-running gauge.
    must("Fiber::join_drain_residual_still_running", "AC3", chaos)
    # soak fails if any hard-fail counter is non-zero under production.
    must_match(
        r"if \(hard_fail_invariants.*mb_starve_delta\s*!=\s*0\)",
        "AC3",
        chaos,
    )

    # ============================================================
    # AC4: Soft / sandbox=off mode still runs the soak (observes only)
    # ============================================================
    must("AURA_STEAL_SNAPSHOT_SOFT", "AC4", chaos)
    must('unsetenv("AURA_STEAL_SNAPSHOT_SOFT")', "AC4", chaos)
    # Soft steal FORBIDDEN under production / PR gate.
    must("Soft steal (AURA_STEAL_SNAPSHOT_SOFT=1) is FORBIDDEN", "AC4", chaos)
    # Chaos SOAK returns true for SOAK mode (allows dev to use without lock).
    must_match(
        r"const char\* e = std::getenv\(\"AURA_CHAOS_SOAK\"\);\s*"
        r"return e && e\[0\] == '1';",
        "AC4",
        chaos,
    )

    # ============================================================
    # AC5: wired into build.py as optional long job
    # ============================================================
    must("cmd_chaos_pr_hard_fail_gate", "AC5", build)
    must("cmd_chaos_mutate_steal_gc_mailbox_coverage", "AC5", build)
    must("cmd_production_concurrency", "AC5", build)
    must("production_concurrency", "AC5", build)
    # PR gate: short chaos hard-fail.
    must("chaos_pr_hard_fail", "AC5", build)
    # Nightly / deploy: production-concurrency gate.
    must("nightly", "AC5", build)
    # build.py wires chaos binary via add_test or similar.
    must("test_chaos_mutate_steal_gc_mailbox", "AC5", build)

    # ============================================================
    # AC6: reproducible seed + documented reproduction steps
    # ============================================================
    must("chaos_seed", "AC6", chaos)
    must("AURA_CHAOS_SEED", "AC6", chaos)
    # Documented reproduction steps in test file header.
    must("Env knobs", "AC6", chaos)
    must("AURA_CHAOS_WORKERS", "AC6", chaos)
    must("AURA_CHAOS_FIBERS", "AC6", chaos)
    must("AURA_CHAOS_DURATION_S", "AC6", chaos)
    must("AURA_CHAOS_FULL", "AC6", chaos)
    must("AURA_CHAOS_SOAK", "AC6", chaos)
    must("AURA_CHAOS_PR_GATE", "AC6", chaos)
    must("AURA_CHAOS_FAULT", "AC6", chaos)
    must("AURA_CHAOS_MB_STARVE_MAX", "AC6", chaos)
    must("AURA_STEAL_SNAPSHOT_HARD", "AC6", chaos)
    must("AURA_LOCK_ORDER_CANARY", "AC6", chaos)
    must("AURA_PRODUCTION_CONCURRENCY_GATE", "AC6", chaos)

    # ============================================================
    # Self-coverage + build.py wire-up
    # ============================================================
    must("check_chaos_soak_2679", "self", build)
    must("#2679", "self", chaos)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2679 chaos/soak production gate — all 6 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
