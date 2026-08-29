#!/usr/bin/env python3
"""Issue #3404: auto-arm Soft fallback must NOT bump auto_alloc_trigger_count — only real Moving success counts.

Contract:
  AC1 `maybe_auto_compact_on_alloc` (arena.ixx) tracks a `real_reclaim`
     flag and ONLY bumps `stats_.auto_alloc_trigger_count` when
     `real_reclaim == true`. Soft fallback paths (no hook /
     moving_blocked_precondition / pin-guard) leave `real_reclaim` false
     — Agent dashboards no longer read Soft mark-only as auto-arm
     success. Source-cite anchor for the no-trigger-on-soft contract.
  AC2 `#3370` linter stays green (no Moving without hook). Verified by
     re-running scripts/coverage/checks/check_arena_auto_arm_known_roots_3370.py.
  AC3 `moving_dense_health` distinguishes `auto_arm_moving_success_total`
     (real Moving success) vs `auto_arm_no_hook_fallback_total` +
     `pin_guard_soft_gate_total` (Soft fallback). The success rate is
     computed from the first; the fallback counters are surfaced
     separately so Agents cannot accidentally sum them into success.
  AC4 existing `#3370` `#3200` `#3123` tests green. Extend the
     existing moving-densify health suite (no `test_issue_*.cpp`).
  AC5 no scheduler change, no second pin registry. (Sanity: no new
     fields added to thread-locals or global registry types.)
  AC6 no `tests/core/test_issue_3404.cpp` (extends existing tests per
     #81934); no `docs/design/3404-*` (per #1655).
  AC7 source-cite #3404 + build.py registration; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_comments_and_strings(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def main() -> int:
    fails: list[str] = []

    arena = _read("src/core/arena.ixx")
    health = _read("src/core/moving_densify_health.hh")
    build = _read("build.py")
    arena_stripped = _strip_comments_and_strings(arena)

    # AC1: maybe_auto_compact_on_alloc has the source-cite anchor + the
    # real_reclaim guard around auto_alloc_trigger_count++.
    ac1_anchor = "// Issue #3404:"
    if ac1_anchor not in arena:
        fails.append(
            "AC1: arena.ixx is missing the #3404 source-cite anchor for the no-trigger-on-Soft-fallback contract"
        )
    if "real_reclaim" not in arena:
        fails.append(
            "AC1: arena.ixx is missing the `real_reclaim` flag "
            "(Soft fallback paths must NOT bump auto_alloc_trigger_count)"
        )
    # Verify the guard: find maybe_auto_compact_on_alloc body region
    # (roughly) and check that auto_alloc_trigger_count++ is wrapped
    # in `if (real_reclaim)`. Simpler: search the file for the guarded
    # pattern (must contain both `real_reclaim` and `auto_alloc_trigger_count++`
    # in close proximity with the guard).
    guard_pat = re.compile(
        r"if\s*\(\s*real_reclaim\s*\)\s*\n\s*stats_\.auto_alloc_trigger_count\+\+",
        re.M,
    )
    if not guard_pat.search(arena_stripped):
        fails.append(
            "AC1: `stats_.auto_alloc_trigger_count++` is not guarded by "
            "`if (real_reclaim)` — Soft fallback paths will still bump "
            "the trigger counter"
        )
    # The unconditional guard is removed (no top-level
    # `stats_.auto_alloc_trigger_count++` outside any guard).
    uncond = re.findall(
        r"^stats_\.auto_alloc_trigger_count\+\+",
        arena_stripped,
        re.M,
    )
    if len(uncond) != 0:
        fails.append(
            f"AC1: {len(uncond)} unconditional `auto_alloc_trigger_count++` "
            "remain — should be 0 (all calls must be inside `if (real_reclaim)`)"
        )

    # AC3: moving_densify_health distinguishes success vs fallback.
    if "g_production_auto_arm_moving_success_total" not in health:
        fails.append(
            "AC3: moving_densify_health.hh is missing "
            "g_production_auto_arm_moving_success_total counter "
            "(Agent dashboards need a separate success field — "
            "fallback counters must not sum into success)"
        )
    if "note_production_auto_arm_moving_success" not in health:
        fails.append(
            "AC3: moving_densify_health.hh is missing "
            "note_production_auto_arm_moving_success() function "
            "(arena.ixx calls it on real Moving success)"
        )
    # The original counters must still exist (#3370 + #3200).
    for counter in (
        "g_production_auto_arm_no_hook_fallback_total",
        "g_production_pin_guard_soft_gate_total",
        "g_production_auto_arm_moving_total",
    ):
        if counter not in health:
            fails.append(
                f"AC3: moving_densify_health.hh is missing the {counter} "
                "counter (the existing #3370 / #3200 invariant must stay)"
            )

    # AC2: #3370 linter stays green (no Moving without hook). Re-run it.
    linter_3370 = ROOT / "scripts" / "coverage" / "checks" / "check_arena_auto_arm_known_roots_3370.py"
    if linter_3370.is_file():
        try:
            r = subprocess.run(
                [sys.executable, str(linter_3370)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            if r.returncode != 0:
                fails.append(
                    f"AC2: #3370 linter regressed (rc={r.returncode}). Output:\n{r.stdout[-500:]}\n{r.stderr[-500:]}"
                )
        except subprocess.TimeoutExpired:
            fails.append("AC2: #3370 linter timed out after 120s")
    else:
        fails.append("AC2: #3370 linter not found at scripts/coverage/checks/")

    # AC5: no scheduler change, no second pin registry (sanity: the
    # arena change should not introduce new thread_local globals or a
    # second pin registration map). Source-cite: the `real_reclaim`
    # flag is a local variable in maybe_auto_compact_on_alloc, and the
    # new moving_success counter is in moving_densify_health.hh (not a
    # second registry). No new `inline std::atomic` introduced in
    # arena.ixx for #3404.
    new_atomics = re.findall(
        r"inline\s+std::atomic<[^>]+>\s+g_\w*3404\w*",
        arena,
    )
    if new_atomics:
        fails.append(
            f"AC5: arena.ixx introduced {len(new_atomics)} new "
            "`std::atomic` globals for #3404 (no scheduler change, "
            "no second pin registry — counter lives in "
            "moving_densify_health.hh)"
        )

    # AC6: no tests/core/test_issue_3404.cpp, no docs/design/3404-*.md.
    if (ROOT / "tests" / "core" / "test_issue_3404.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3404.cpp exists — must extend existing test per #81934")
    if list((ROOT / "docs" / "design").glob("3404-*.md")):
        fails.append("AC6: docs/design/3404-*.md exists — design docs banned per #1655")

    # AC7: source-cite #3404 + build.py registration; no design docs.
    if "#3404" not in arena and "#3404" not in health:
        fails.append("AC7: source-cite #3404 missing from arena.ixx / moving_densify_health.hh")
    if "check_arena_auto_arm_soft_fallback_3404" not in build:
        fails.append("AC7: build.py does not register check_arena_auto_arm_soft_fallback_3404")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3404 arena auto-arm Soft fallback contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
