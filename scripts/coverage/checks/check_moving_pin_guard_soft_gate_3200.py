#!/usr/bin/env python3
"""Issue #3200: production Moving pin/EnvFrame Soft-gate arms sticky.

When production pack is active and live pins or EnvFrame guards block
Moving, arm existing sticky densify-off + Agent throttle so the outer
loop cannot pretend densify amortisation occurred. Soft observe-only.

Contract:
  AC1 Production + pins/guards → sticky + throttle + health face
  AC2 Soft / no-pins path unchanged
  AC3 Incomplete-remap / untracked still fail-closed + sticky
  AC4 Quiet residual==0 zero extra (arm only on pin/guard block)
  AC5 Extend moving densify health suite; this linter; no invent/docs
  AC6 Source-cite the production Soft-gate → sticky decision site

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _extract_fn_body(src: str, sig_pat: str) -> str | None:
    m = re.search(sig_pat, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    arena = _read("src/core/arena.ixx")
    hh = _read("src/core/moving_densify_health.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_arena_moving_densify_health.cpp")
    build = _read("build.py")

    must("kMovingPinGuardSoftGateIssue = 3200", "AC1 stamp arena", arena)
    must("kMovingPinGuardSoftGateIssue = 3200", "AC1 stamp health", hh)
    must("arm_production_pin_guard_soft_gate", "AC1 arm", arena)
    must("production_moving_wanted_but_pin_or_guard", "AC1 auto path", arena)
    must("note_production_pin_guard_soft_gate", "AC1 health", hh)
    must("note_agent_throttle_for_moving_densify", "AC1 throttle", hh)
    must("ac3200_1_production_pin_sticky_throttle", "AC1 test", t)

    compact = _extract_fn_body(arena, r"live_compact\s*\(\s*LiveCompactMode\s+mode")
    if not compact:
        fails.append("AC1: could not extract live_compact")
    else:
        if "arm_production_pin_guard_soft_gate" not in compact:
            fails.append("AC1: live_compact missing arm_production_pin_guard_soft_gate")
        if "production_auto_arm_pack_active()" not in compact:
            fails.append("AC6: live_compact missing production pack gate")
        must("Issue #3200", "AC6 cite", compact)

    must("ac3200_2_soft_observe_only", "AC2 test", t)
    must("Soft/sandbox observe-only", "AC2 comment", arena)

    must("g_moving_incomplete_remap_sticky_densify_off", "AC3 sticky", arena)
    must("ac3200_3_untracked_fail_closed_unchanged", "AC3 test", t)

    must("ac3200_4_quiet_zero_extra", "AC4 test", t)
    must("pin_block || guard_block", "AC4 quiet", arena)

    must("schema-3200", "AC5 query", q)
    must("pin-or-guard-soft-gate-total", "AC5 query key", q)
    must("ac3200_5_source_and_linter", "AC5 test", t)
    must("check_moving_pin_guard_soft_gate_3200", "AC5 build.py", build)
    if "query:pin-or-guard-soft-gate" in q:
        fails.append("AC5: new public query key")

    if (ROOT / "tests" / "compiler" / "test_issue_3200.cpp").is_file():
        fails.append("AC5: test_issue_3200.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3200.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3200.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3200-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3200 production pin/EnvFrame Soft-gate sticky — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
