#!/usr/bin/env python3
"""Issue #2513: production-grade multi-fiber soak extension for concurrency gate.

Contract:
  AC1 Configurable duration/fibers (AURA_CHAOS_DURATION_S / FIBERS / SOAK)
  AC2 Hard-fail criteria: steal hard-fail, residual still-running, mailbox
      starvation ceiling, hang
  AC3 reclaim residual still-running path (#2397 / #2467)
  AC4 coverage linter + source-cite for soak paths
  AC5 production-concurrency docs / knobs updated

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
    build = _read("build.py")
    nightly = _read(".github/workflows/nightly.yml")
    gate2380 = _read("scripts/check_production_concurrency_gate_2380.py")

    # AC1 configurable soak
    must("AURA_CHAOS_SOAK", "AC1", test)
    must("AURA_CHAOS_FIBERS", "AC1", test)
    must("AURA_CHAOS_DURATION_S", "AC1", test)
    must("ac2513_soak_optional", "AC1", test)
    must("Issue #2513", "AC1", test)

    # AC2 hard-fail criteria
    must("steal_snapshot_hard_fail", "AC2", test)
    must("join_drain_residual_still_running", "AC2", test)
    must("AURA_CHAOS_MB_STARVE_MAX", "AC2", test)
    must("mailbox_hold_exit_starvation", "AC2", test)
    must("no hang", "AC2", test)

    # AC3 reclaim residual
    must("ac2513_reclaim_residual_still_running", "AC3", test)
    must("mark_reclaimed", "AC3", test)
    must("note_body_exit_if_reclaimed", "AC3", test)
    must("maybe_reap_orphans_on_tick", "AC3", test)

    # AC4 source-cite / linter
    must("non_yield_spins", "AC4", test)
    must("ac2513_docs_and_source", "AC4", test)
    must("check_production_concurrency_soak_2513", "AC4", build)
    must("cmd_production_concurrency_soak_coverage", "AC4", build)

    # AC5 docs / gate wiring
    must("AURA_CHAOS_SOAK", "AC5", build)
    must("production-concurrency", "AC5", build)
    must("2513", "AC5", build)
    must("AURA_CHAOS_FULL", "AC5", nightly)
    must("production-concurrency", "AC5", nightly)
    # Lineage #2380 still present
    must("AURA_PRODUCTION_CONCURRENCY_GATE", "AC5", gate2380)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2513 production-concurrency soak extension — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
