#!/usr/bin/env python3
"""Issue #2636: residual reclaim observability — body-age + env-opt-in force-safepoint.

Contract:
  AC1 mark_reclaimed sets body_reclaim_start_ns; body exit observes age_ms_max > 0
  AC2 note_body_exit_if_reclaimed + ~Fiber update max (CAS) / sum / samples
  AC3 Soft default: env AURA_FORCE_FIBER_SAFEPOINT_ON_ORPHAN default ON (preserve #2533)
  AC4 Env opt-in force-safepoint path source-cited + metric counter
  AC5 query:orch-module-stats keys + schema-2636 / issue-2636 / wired sentinels
  AC6 src-aligned tests (test_residual_force_safepoint_2533 + test_orch_obs_facade_2589)
     + this linter wired into build.py

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
    orch = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test_2533 = _read("tests/serve/test_residual_force_safepoint_2533.cpp")
    test_2589 = _read("tests/orch/test_orch_obs_facade_2589.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1: body-age lifecycle (mark_reclaimed records start timestamp)
    must("Issue #2636", "AC1", fh)
    must("body_reclaim_start_ns_", "AC1", fh)
    must("set_body_reclaim_start_ns", "AC1", fh)
    must("join_drain_residual_body_age_ms_max", "AC1", fh)
    must("Issue #2636: record body-age start timestamp", "AC1", fc)
    must("body_reclaim_start_ns_.store(now_ns", "AC1", fc)
    must("2636 AC1", "AC1", test_2533)

    # AC2: note_body_exit_if_reclaimed + ~Fiber update age stats (max via CAS)
    must("2636: finalize body-age", "AC2", fc)
    must("join_drain_residual_body_age_ms_max_.compare_exchange_weak", "AC2", fc)
    must("join_drain_residual_body_age_samples_", "AC2", fc)
    must("join_drain_residual_body_age_ms_max", "AC2", orch)
    must("join_drain_residual_body_age_ms_sum", "AC2", orch)
    must("join_drain_residual_body_age_samples", "AC2", orch)
    must("note_residual_body_age_ms", "AC2", orch)
    must("2636 AC2", "AC2", test_2533)

    # AC3: Soft default = env ON preserves #2533 behavior
    must("AURA_FORCE_FIBER_SAFEPOINT_ON_ORPHAN", "AC3", mut)
    must("resolve_force_safepoint_on_orphan_enabled", "AC3", orch)
    must("force_safepoint_on_orphan_enabled", "AC3", fh)
    must("2636 AC3", "AC3", test_2533)

    # AC4: env opt-in force-safepoint path source-cited + metric counter
    must("aura_force_safepoint_on_orphan_enabled_default", "AC4", fc)
    must("aura_orch_bump_force_safepoint_on_orphan_total", "AC4", fc)
    must("aura_orch_bump_force_safepoint_on_orphan_total", "AC4", mut)
    must("aura_orch_note_residual_body_age_ms", "AC4", mut)
    must("aura_orch_note_residual_body_age_ms", "AC4", fc)
    must("force_safepoint_on_orphan_total", "AC4", orch)
    must("bump_force_safepoint_on_orphan_total", "AC4", orch)
    must("2636 AC4", "AC4", test_2533)

    # AC5: query keys + schema + wired sentinels
    must("join-drain-residual-body-age-ms-max", "AC5", prim)
    must("join-drain-residual-body-age-ms-sum", "AC5", prim)
    must("join-drain-residual-body-age-samples", "AC5", prim)
    must("force-safepoint-on-orphan-total", "AC5", prim)
    must("force-safepoint-on-orphan-enabled", "AC5", prim)
    must("schema-2636", "AC5", prim)
    must("issue-2636", "AC5", prim)
    must("residual-body-age-wired", "AC5", prim)
    must("force-safepoint-on-orphan-wired", "AC5", prim)
    must("2636 AC5", "AC5", test_2589)

    # AC6: tests + cmake + build.py gate
    must("2636 AC6", "AC6", test_2533)
    must("check_residual_body_age_coverage", "AC6", build)
    must("cmd_residual_body_age_coverage", "AC6", build)
    must("test_residual_force_safepoint_2533", "AC6", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2636 residual body-age — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
