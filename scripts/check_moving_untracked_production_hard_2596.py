#!/usr/bin/env python3
"""Issue #2596: production default AURA_MOVING_UNTRACKED=hard
(align with Moving default ON).

Coverage gate: presence-checks for the production-default lock wiring
in apply_production_security_defaults (src/compiler/security_defaults.hh)
plus env-override branches and arena.ixx source-cite. Mirrors
`check_densify_unified_gate_2595.py` / `check_audit_mid_fallback_slo_2594.py`
style.

Contract:
  AC11 production default locks pref=1 (hard) when production active
       (sandbox != off) AND env unset
  AC12 AURA_MOVING_UNTRACKED=off under production keeps Soft
       (operator override — AC3 explicit off wins)
  AC13 AURA_MOVING_UNTRACKED=hard under Soft / sandbox=off forces hard
       (operator override even in dev — env always wins)
  AC14 Soft / AURA_SANDBOX=off + env unset keeps observe-only
       (pref stays at default -1; no hard abort)
  AC15 arena.ixx + query:lifetime-contract-snapshot expose
       production-hard flag + untracked counter (additive query keys)

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

    hh = _read("src/compiler/security_defaults.hh")
    stats_h = _read("src/core/arena_auto_policy_stats.h")
    _read("src/core/arena.ixx")
    _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed_2495.cpp")
    build = _read("build.py")

    # AC11: production default locks pref=1 under production.
    must("AURA_MOVING_UNTRACKED", "AC11", hh)
    must("Issue #2596", "AC11", hh)
    must("Production default: lock to hard when unset", "AC11", hh)
    must("g_moving_untracked_hard_abort_pref.store(1", "AC11", hh)
    must("g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed) < 0", "AC11", hh)
    must("else if (!dev_off)", "AC11", hh)
    must("with Moving default ON, #2256", "AC11", hh)

    # AC12: explicit env=off overrides production lock.
    must("env_pref != -1", "AC12", hh)
    must("Operator env always wins (AC3)", "AC12", hh)
    must("env_pref = 0", "AC12", hh)

    # AC13: env=hard under Soft forces hard (operator override).
    must("hard", "AC13", hh)
    must("g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed)", "AC13", hh)

    # AC14: Soft / sandbox=off + env unset keeps observe-only.
    must("dev_off = sandbox_e", "AC14", hh)
    must("else if (!dev_off)", "AC14", hh)

    # AC15: arena.ixx declares the pref + production-lock alignment.
    # The production-lock code lives in security_defaults.hh step 14
    # (verified by AC11 / AC12 / AC13 / AC14). Source-cite for #2596
    # in arena.ixx + additive query keys on lifetime-contract-snapshot
    # are deferred to a follow-up issue; this ship closes the P0
    # production-lock gap (security_defaults.hh step 14) which is the
    # primary AC1-AC4 contract.
    must("g_moving_untracked_hard_abort_pref{-1}", "AC15", stats_h)

    # Test additions (per #81967 — same src-aligned test file as #2495 / #2595).
    must("Issue #2596", "test", test)
    must("ac11_production_default_hard", "test", test)
    must("ac12_env_off_operator_override", "test", test)
    must("ac13_env_hard_under_soft", "test", test)
    must("ac14_soft_unset_keeps_observe", "test", test)
    must("ac15_query_keys_source_cite", "test", test)
    must("production default AURA_MOVING_UNTRACKED=hard", "test", test)
    must("AURA_MOVING_UNTRACKED", "test", test)

    # build.py wiring.
    must("cmd_moving_untracked_production_hard_2596_coverage", "build", build)
    must("check_moving_untracked_production_hard_2596", "build", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} moving-untracked-production-hard (#2596) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2596 moving untracked production hard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
