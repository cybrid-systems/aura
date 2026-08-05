#!/usr/bin/env python3
"""Issue #2667: production-only hard residual GcDefer on steal-complete +
PanicCheckpoint rebind (end Soft leftover).

Contract (one row per AC):
  AC1 src/compiler/evaluator_fiber_mutation.cpp aura_evaluator_on_steal_complete
     under production_defaults / Hard (is_steal_snapshot_hard_mode() +
     aura_production_defaults_active_probe() per #2546) clears live
     PanicCheckpoint on the residual eval + bumps
     g_panic_checkpoint_cleared_on_steal_total. Soft / dev_off: no
     action (preserve Soft semantics — no clear, no counter bump).
  AC2 src/core/gc_hooks.h declares g_panic_checkpoint_cleared_on_steal_total
     process-wide atomic (default 0) + getter
     panic_checkpoint_cleared_on_steal_total() mirroring the
     g_residual_defer_steal_* axis (production vs Soft).
  AC3 src/compiler/evaluator_primitives_obs_jit.cpp exposes additive
     query sentinels: panic-checkpoint-cleared-on-steal-total +
     panic_checkpoint_cleared_on_steal_total + panic-checkpoint-cleared-
     on-steal-wired + schema-2667 + issue-2667. #2546 / #2314 / #2203
     surfaces preserved (additive — no break).
  AC4 tests/serve/test_residual_defer_steal_hard_and.cpp extended with
     #2667 AC1-AC5 source-cite block (per #81967 — no new issue-suffix
     file).
  AC5 build.py wires check_2667_coverage into the gate after
     check_2666_coverage.
  AC6 cross-check: check_steal_complete_strong_entry_2377 +
     check_residual_sid0_cap_coverage still green (no regression).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    gc = _read("src/core/gc_hooks.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_residual_defer_steal_hard_and.cpp")
    build = _read("build.py")

    # AC1 — production-default panic_checkpoint clear on steal
    must("Issue #2667", "AC1", efm)
    must("has_panic_checkpoint", "AC1", efm)
    must("clear_panic_checkpoint", "AC1", efm)
    must("g_panic_checkpoint_cleared_on_steal_total.fetch_add", "AC1", efm)

    # AC2 — counter declaration + getter in gc_hooks.h
    must("g_panic_checkpoint_cleared_on_steal_total{0}", "AC2", gc)
    must("panic_checkpoint_cleared_on_steal_total", "AC2", gc)

    # AC3 — additive query sentinels in obs_jit.cpp
    must("panic-checkpoint-cleared-on-steal-total", "AC3", obs)
    must("panic_checkpoint_cleared_on_steal_total", "AC3", obs)
    must("panic-checkpoint-cleared-on-steal-wired", "AC3", obs)
    must("schema-2667", "AC3", obs)
    must("issue-2667", "AC3", obs)
    # #2546 / #2314 / #2203 surfaces preserved (regression)
    must("schema-2546", "AC3", obs)
    must("schema-2314", "AC3", obs)
    must("residual-defer-cleared-on-steal-total", "AC3", obs)

    # AC4 — test file extension
    must("ac2667_1_production_panic_checkpoint_clear", "AC4", test)
    must("ac2667_2_query_sentinel_source_cite", "AC4", test)
    must("ac2667_3_coverage_linter_wired", "AC4", test)
    must("Issue #2667", "AC4", test)

    # AC5 — build.py wires the linter
    must("check_2667_coverage", "AC5", build)

    # Cross-check: check_steal_complete_strong_entry_2377 still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_steal_complete_strong_entry_2377.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_steal_complete_strong_entry_2377 regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: check_residual_sid0_cap_coverage still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_residual_sid0_cap_coverage.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_residual_sid0_cap_coverage regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2667 production-only hard residual GcDefer on steal-complete + PanicCheckpoint rebind — all AC rows satisfied"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
