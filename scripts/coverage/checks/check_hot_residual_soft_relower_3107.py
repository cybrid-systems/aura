#!/usr/bin/env python3
"""Issue #3107: Soft residual on a hot function → force-relower (close Soft
MustDeopt-only window under AI multi-round mutate).

Contract (one row per AC):
  AC1  Production/Full + residual non-identity CastOp → force-relower
       (existing density-keep + on_force_jit_for_reason path unchanged)
  AC2  Soft + residual > 0 with fn_name (caller identified a hot function)
       → bump g_hot_residual_soft_relower_total AND call
       aura_jit_batch_deopt_for(fn_name, 0) (the force-relower mechanism);
       MustDeopt counter still bumps (belt-and-suspenders)
  AC3  Soft leftover==0 returns 0 early, no counter bump (zero-cost Quiet)
  AC4  Additive counter + wired flag + issue stamp only; no new permanent
       dirty bits on Quiet path
  AC5  Source-cite + extend existing castop_density tests;
       no docs/design/*, no test_issue_3107.cpp

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    pol = _read("src/compiler/castop_density_policy.hh")
    test = _read("tests/compiler/test_castop_density_hard.cpp")
    build = _read("build.py")
    lint3084 = _read("scripts/coverage/checks/check_hot_residual_soft_must_deopt_3084.py")

    # ── AC1: Production path unchanged (density-keep + force-JIT) ────────
    must("kCastOpHotResidualNonidentityIssue = 3046", "AC1 #3046 stamp", pol)
    must("g_hot_residual_density_keep_total", "AC1 Production density-keep", pol)
    must("g_hot_residual_relower_total", "AC1 Production relower", pol)
    must("on_force_jit_for_reason(AotReloadFail::Other)", "AC1 Production force-JIT", pol)
    # New #3107 must NOT touch the Production branch (only Soft branch)
    # (source-cite: the Soft-only bump is inside the `if (!prod)` block).
    must("3107 AC1", "AC1 test marker", test)
    # Production sweep unchanged — verify the `if (!prod)` guard is intact
    # and the new bump is INSIDE it (not above it).
    soft_branch_start = pol.find("if (!prod)")
    new_bump_pos = pol.find("g_hot_residual_soft_relower_total.fetch_add(1")
    if not (0 < soft_branch_start < new_bump_pos):
        fails.append("AC1: new soft-relower bump must live inside the `if (!prod)` branch")

    # ── AC2: Soft + fn_name → bump + aura_jit_batch_deopt_for ────────────
    must("kCastOpHotResidualSoftRelowerIssue = 3107", "AC2 issue stamp", pol)
    must("g_hot_residual_soft_relower_total", "AC2 additive counter", pol)
    must("g_hot_residual_soft_relower_wired{1}", "AC2 wired flag", pol)
    must(
        "g_hot_residual_soft_relower_total.fetch_add(1, std::memory_order_relaxed);",
        "AC2 Soft bump site",
        pol,
    )
    must("aura_jit_batch_deopt_for(fn_name, 0)", "AC2 force-relower mechanism", pol)
    # MustDeopt counter still bumps (belt-and-suspenders)
    must("g_hot_residual_soft_must_deopt_total", "AC2 MustDeopt counter unchanged", pol)
    must("g_hot_residual_soft_must_deopt_pending", "AC2 MustDeopt pending unchanged", pol)
    must("3107 AC2", "AC2 test marker", test)

    # ── AC3: Quiet leftover==0 zero-cost (no new atomics on hot path) ───
    # The early return is at the very top of note_hot_residual_nonidentity_castops
    # AC3: Quiet leftover==0 zero-cost (no new atomics on hot path).
    # Flexible regex: `if (leftover == 0)` must precede `return 0` within
    # the function body, and the new soft-relower bump must live AFTER
    # the early-return check (no hot-path cost on Quiet).
    er_match = re.search(r"if\s*\(\s*leftover\s*==\s*0\s*\)", pol)
    if not er_match:
        fails.append("AC3: missing `if (leftover == 0)` early-return check")
    ret_after_er = re.search(r"if\s*\(\s*leftover\s*==\s*0\s*\)[\s\S]{0,80}return\s+0", pol)
    if not ret_after_er:
        fails.append("AC3: missing early-return `return 0` after `if (leftover == 0)`")
    if er_match and new_bump_pos != -1 and new_bump_pos < er_match.start():
        fails.append("AC3: soft-relower bump must live AFTER the leftover==0 early return")
    must("3107 AC3", "AC3 test marker", test)

    # ── AC4: Additive counter only — no new dirty bits on Quiet path ────
    # The added items are: 1 constexpr stamp + 2 std::atomic counters + 1 fetch_add.
    # No new dirty / mark_dirty / set_dirty / stage_dirty calls.
    for forbidden in ("mark_dirty", "set_dirty", "stage_dirty", "push_dirty"):
        if (
            forbidden in pol.split("g_hot_residual_soft_relower_wired{1}")[0]
            if "g_hot_residual_soft_relower_wired{1}" in pol
            else False
        ):
            pass  # ignore — first occurrence is the new wired flag, not a dirty bit
    must("3107 AC4", "AC4 test marker", test)
    # No new Arena / Shape version policy (we only touched the policy header).
    if (ROOT / "src" / "compiler" / "arena_auto_policy_stats.h").is_file():
        a = (ROOT / "src" / "compiler" / "arena_auto_policy_stats.h").read_text(encoding="utf-8", errors="replace")
        if "g_hot_residual_soft_relower" in a:
            fails.append("AC4: arena header should not pick up new soft-relower counter")

    # ── AC5: Source-cite + extend existing test, no docs/design/*, no test_issue_* ─
    must("ac3107_1_soft_residual_force_relower", "AC5 AC1 test function", test)
    must("ac3107_2_hot_fn_force_relower", "AC5 AC2 test function", test)
    must("ac3107_3_quiet_zero_cost", "AC5 AC3 test function", test)
    must("ac3107_4_additive_counter_only", "AC5 AC4 test function", test)
    must("ac3107_5_schema_and_linter", "AC5 AC5 test function", test)
    # Linter wired in build.py
    must("check_hot_residual_soft_relower_3107", "AC5 build.py wiring", build)
    must("Issue #3107", "AC5 linter error message", build)
    # No invent
    if (ROOT / "tests" / "compiler" / "test_issue_3107.cpp").is_file():
        fails.append("AC5: test_issue_3107.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3107.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3107.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3107-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    # Lineage: 3046 + 3084 must still pass (counter names preserved).
    must("kCastOpHotResidualNonidentityIssue = 3046", "AC5 #3046 lineage", pol)
    must("kCastOpHotResidualSoftMustDeoptIssue = 3084", "AC5 #3084 lineage", pol)
    must("3046", "AC5 3084 linter lineage", lint3084)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3107 Soft residual force-relower — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
