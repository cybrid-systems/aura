#!/usr/bin/env python3
"""Issue #2619: Agent-visible Moving densify health (pairs #2596).

Contract:
  AC1 Query exposes pin_contract / untracked / production-hard after densify window
  AC2 Production + incomplete remap → would-allow-mutate = false
  AC3 Soft/sandbox observe-only unless #2596 hard active
  AC4 No densify → vacuous healthy / zero extra cost
  AC5 Schema additive; source-cite #2596 / #2495 / #2619

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

    hh = _read("src/core/moving_densify_health.hh")
    boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    arena = _read("src/core/arena.ixx")
    sec = _read("src/compiler/security_defaults.hh")
    test = _read("tests/compiler/test_arena_moving_densify_health_2619.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("query:arena-moving-densify-health", "AC1", q)
    must("pin-contract-held", "AC1", q)
    must("untracked-kept", "AC1", q)
    must("production-hard-active", "AC1", q)
    must("publish_last_moving_densify_window", "AC1", hh)
    must("publish_last_moving_densify_window", "AC1", boundary)
    must("ac1_query_exposes_window", "AC1", test)

    # AC2
    must("would-allow-mutate", "AC2", q)
    must("would_allow_mutate", "AC2", hh)
    must("kForceUntrackedIncomplete", "AC2", hh)
    must("ac2_incomplete_denies_mutate", "AC2", test)

    # AC3 soft observe-only unless hard
    must("production_hard_active", "AC3", hh)
    must("g_moving_untracked_hard_abort_pref", "AC3", hh)
    must("agent_throttle_for_moving_densify", "AC3", hh)
    must("Issue #2596", "AC3", sec)
    must("ac3_soft_observe_only", "AC3", test)

    # AC4 vacuous healthy
    must("vacuous", "AC4", hh)
    must("ac4_no_densify_healthy", "AC4", test)
    must("reset_moving_densify_health_for_test", "AC4", hh)

    # AC5
    must("#2619", "AC5", hh)
    must("#2495", "AC5", hh)
    must("schema-2619", "AC5", q)
    must("schema-2596", "AC5", q)
    must("schema-2495", "AC5", q)
    must("moving_incomplete_remap_any", "AC5", arena)
    must("untracked_kept_total", "AC5", arena)
    must("test_arena_moving_densify_health_2619", "AC5", cmake)
    must("check_arena_moving_densify_health_2619", "AC5", build)
    must("cmd_arena_moving_densify_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    for rel in (
        "docs/design/arena_moving_densify_health_2619.md",
        "docs/arena_moving_densify_health_2619.md",
        "design/2619.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2619 arena moving densify health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
