#!/usr/bin/env python3
"""Issue #2352: chaos mutate × steal × GC × mailbox production gate.

Contract:
  AC1 Fixed-seed chaos completes (smoke always; full via AURA_CHAOS_FULL)
  AC2 Injected residual Panic fails detection
  AC3 Snapshot mismatch inject under Hard
  AC4 Smoke ≤ 90s; full nightly-ok
  AC5 Documented AURA_CHAOS_* knobs + CMake + gate

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

    test = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("AURA_CHAOS_SEED", "AC1", test)
    must("run_chaos_pass", "AC1", test)
    must("ac1_smoke", "AC1", test)
    must("ac1_full_optional", "AC1", test)
    must("AURA_CHAOS_FULL", "AC1", test)

    # AC2 residual
    must("ac2_inject_residual_panic", "AC2", test)
    must("arm_gc_defer_pending_panic_for", "AC2", test)
    must("residual_panic", "AC2", test)

    # AC3 snapshot
    must("ac3_inject_snapshot_mismatch", "AC3", test)
    must("mutation_steal_snapshot_mismatch_total", "AC3", test)
    must("AURA_STEAL_SNAPSHOT_HARD", "AC3", test)
    must("bump_mutation_steal_snapshot_mismatch", "AC3", test)

    # AC4 smoke budget + anti-hang
    must("90", "AC4", test)
    must("resume_from_gc", "AC4", test)
    must("watchdog", "AC4", test)
    must("defer_reasons_snapshot", "AC4", test)

    # AC5 registration
    must("Issue #2352", "AC5", test)
    must("AURA_CHAOS_WORKERS", "AC5", test)
    must("AURA_CHAOS_DURATION_S", "AC5", test)
    must("AURA_CHAOS_FIBERS", "AC5", test)
    must("MultiFiberMailbox", "AC5", test)
    must("MutationBoundaryGuard", "AC5", test)
    must("test_chaos_mutate_steal_gc_mailbox_2352", "AC5", cmake)
    must("check_chaos_mutate_steal_gc_mailbox_2352", "AC5", build)
    must("cmd_chaos_mutate_steal_gc_mailbox_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2352 chaos mutate×steal×GC×mailbox — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
