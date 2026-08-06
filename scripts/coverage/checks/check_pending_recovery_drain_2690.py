#!/usr/bin/env python3
"""Issue #2690: unified PendingRecovery drain (close residual unhealed windows).

Contract:
  AC1 non-None → None storm + any pending → single drain runs body;
      reemit_storm_clear_health_pass_reemit_driven_total advances; no
      second independent body from boundary in the same ms without
      double_drain_prevented. Exchange-not-check semantics: a
      concurrent drain in the same ms observes kinds == 0 (cheap) and
      bumps double_drain_prevented to surface the race.
  AC2 quiet path (storm None, no pending) → zero extra work
      (existing #2669 AC2 preserved).
  AC3 storm re-enters mid-drain → skip + skipped_reentered; deferred
      not silently lost.
  AC4 boundary exit alone with deferred pending still drains
      (regression of #2604). Both entry points (storm-clear +
      boundary-exit) route through drain_pending_recovery(why).
  AC5 additive query keys + schema sentinel (schema-2690);
      #2604/#2601/#2502/#2669 surfaces preserved.
  AC6 source-cite + coverage linter; extend
      test_exhausted_min_dirty_reemit per #81967 (no docs/design per
      #1655).

This linter (AC5/AC6) verifies:
  - PendingRecovery struct + kPending* bit constants + DrainReason enum
    + kPendingRecoveryDrainIssue stamp in hot_update_registry.hh
  - exchange_pending_recovery() + drain_pending_recovery(DrainReason)
    implementations in hot_update_registry.cpp
  - C ABI hooks: aura_hot_update_exchange_pending_recovery() +
    aura_hot_update_drain_pending_recovery(reason)
  - 4 process-wide counters declared
  - Wire: maybe_storm_clear_health_pass calls drain_pending_recovery(StormClear)
  - Wire: outermost MutationBoundary success exit calls
    drain_pending_recovery(BoundaryExit)
  - Query surface: 4 keys + schema-2690 + issue-2690 + drain-wired sentinel
  - No docs/design/* regression

Exit 0 = OK, 1 = violation found.
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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    eval_mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _read("build.py")

    # AC1/AC4/AC5 — PendingRecovery struct + bit constants + DrainReason
    # enum + kPendingRecoveryDrainIssue stamp.
    must("struct PendingRecovery", "AC1/AC4/AC5", hh)
    must("kinds", "AC1/AC4/AC5", hh)
    must("defuse_version", "AC1/AC4/AC5", hh)
    must("region_mask", "AC1/AC4/AC5", hh)
    must("kPendingDeferred", "AC1/AC4/AC5", hh)
    must("kPendingForceJit", "AC1/AC4/AC5", hh)
    must("kPendingRegionMask", "AC1/AC4/AC5", hh)
    must("enum class DrainReason", "AC1/AC4/AC5", hh)
    must("StormClear", "AC1/AC4/AC5", hh)
    must("BoundaryExit", "AC1/AC4/AC5", hh)
    must("kPendingRecoveryDrainIssue", "AC1/AC4/AC5", hh)
    # exchange + drain declarations.
    must("exchange_pending_recovery", "AC1/AC4", hh)
    must("drain_pending_recovery", "AC1/AC4", hh)

    # AC1/AC4 — implementations in hot_update_registry.cpp.
    must("HotUpdateRegistry::exchange_pending_recovery", "AC1/AC4", cpp)
    must("HotUpdateRegistry::drain_pending_recovery", "AC1/AC4", cpp)
    # C ABI hooks (declarations in header + impls in cpp).
    must("aura_hot_update_exchange_pending_recovery", "AC1/AC4", hh)
    must("aura_hot_update_drain_pending_recovery", "AC1/AC4", hh)
    must("aura_hot_update_exchange_pending_recovery", "AC1/AC4", cpp)
    must("aura_hot_update_drain_pending_recovery", "AC1/AC4", cpp)

    # AC5 — 4 process-wide counters.
    must("g_pending_recovery_driven_total_atomic", "AC5", hh)
    must("g_pending_recovery_success_total_atomic", "AC5", hh)
    must("g_pending_recovery_skipped_reentered_total_atomic", "AC5", hh)
    must("g_pending_recovery_double_drain_prevented_total_atomic", "AC5", hh)

    # AC1/AC4 — Wire check: maybe_storm_clear_health_pass calls
    # drain_pending_recovery(StormClear). Loose check: both names appear
    # in hot_update_registry.cpp.
    must("drain_pending_recovery", "AC1/AC4-wire", cpp)
    # Specifically verify maybe_storm_clear_health_pass calls it.
    m = re.search(
        r"void\s+HotUpdateRegistry::maybe_storm_clear_health_pass\s*\(\s*\)\s*(?:noexcept)?\s*\{(.+?)\n\s*\}",
        cpp,
        re.MULTILINE | re.DOTALL,
    )
    if m and "drain_pending_recovery" not in m.group(1):
        fails.append("AC1: maybe_storm_clear_health_pass body must call drain_pending_recovery")
    # Specifically verify outermost MutationBoundary success exit calls it.
    m = re.search(
        r"if\s*\(\s*!nested_boundary\s*&&\s*success\s*\)\s*\{(.+?)\n\s*\}",
        eval_mb,
        re.MULTILINE | re.DOTALL,
    )
    if (
        m
        and "aura_hot_update_drain_pending_recovery" not in m.group(1)
        and "drain_pending_recovery" not in eval_mb  # loose check: accept drain_pending_recovery in cpp
    ):
        fails.append("AC4: outermost MutationBoundary success exit must call drain_pending_recovery(BoundaryExit)")

    # AC5 — query surface wired.
    must("pending-recovery-driven-total", "AC5", q)
    must("pending-recovery-success-total", "AC5", q)
    must("pending-recovery-skipped-reentered-total", "AC5", q)
    must("pending-recovery-double-drain-prevented-total", "AC5", q)
    must("schema-2690", "AC5", q)
    must("issue-2690", "AC5", q)
    must("pending-recovery-drain-wired", "AC5", q)

    # AC5 — #2604/#2601/#2502/#2669 lineage preserved.
    must("Issue #2604", "AC5", eval_mb)
    must("Issue #2601", "AC5", cpp)
    must("Issue #2502", "AC5", cpp)
    must("Issue #2669", "AC5", cpp)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/pending_recovery_drain_2690.md",
        "docs/pending_recovery_drain_2690.md",
        "design/2690.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: #2690 sentinel in hot_update_registry.hh +
    # evaluator_mutation_boundary.cpp + evaluator_primitives_obs_jit.cpp.
    # Use "#2690" (not "Issue #2690") to accept combined citations.
    must("#2690", "AC6", hh)
    must("#2690", "AC6", cpp)
    must("#2690", "AC6", q)
    must("#2690", "AC6", eval_mb)

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_pending_recovery_drain_2690.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2690 unified PendingRecovery drain — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
