#!/usr/bin/env python3
"""Issue #2372: production hard-forbid Soft steal-snapshot + force-deopt ABI.

Contract:
  AC1 Soft env ignored under production lock; mismatch still force-deopts
  AC2 Production + missing/weak force-deopt ABI → abort (never silent continue)
  AC3 AURA_SANDBOX=off / test override → Soft still usable
  AC4 Happy path: single relaxed load of production-locked flag
  AC5 Query sentinel + schema-2372 + unit test + this linter in pre-push gate

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
    wc = _read("src/serve/worker.cpp")
    sd = _read("src/compiler/security_defaults.hh")
    fb = _read("src/compiler/fiber_bridge.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/serve/test_steal_snapshot_soft_production_lock_2372.cpp")
    cmake = _read("CMakeLists.txt")
    bp = _read("build.py")

    # AC1 Soft ignored under production lock
    must("set_steal_snapshot_soft_production_locked", "AC1", fh)
    must("steal_snapshot_soft_production_locked", "AC1", fh)
    must("g_steal_snapshot_soft_production_locked", "AC1", fc)
    must("is_steal_snapshot_soft_mode", "AC1", fc)
    must("Issue #2372", "AC1", fc)
    must("set_steal_snapshot_soft_production_locked(!dev_off)", "AC1", sd)
    must("AC1:", "AC1", test)

    # AC2 missing ABI fail-closed
    must("steal_snapshot_soft_production_locked()", "AC2", wc)
    must("std::abort()", "AC2", wc)
    must("Issue #2372", "AC2", wc)
    must("steal_snapshot_soft_production_locked()", "AC2", fb)
    must("std::abort()", "AC2", fb)
    must("Issue #2372", "AC2", fb)
    must("AC2:", "AC2", test)

    # AC3 test override / sandbox=off Soft still usable
    must("set_steal_snapshot_soft_for_test", "AC3", fh)
    must("reset_steal_snapshot_soft_for_test", "AC3", fh)
    must("set_steal_snapshot_soft_for_test", "AC3", fc)
    must("AC3:", "AC3", test)

    # AC4 happy path note (relaxed load / rare path only)
    must("relaxed", "AC4", fh.lower())
    must("AC4:", "AC4", test)

    # AC5 query + linter + cmake + gate
    must("schema-2372", "AC5", q)
    must("issue-2372", "AC5", q)
    must("steal-snapshot-soft-forbidden-wired", "AC5", q)
    must("steal-snapshot-soft-production-locked", "AC5", q)
    must("test_steal_snapshot_soft_production_lock_2372", "AC5", cmake)
    must("cmd_steal_snapshot_soft_production_lock_coverage", "AC5", bp)
    must("AC5:", "AC5", test)
    must("Issue #2372", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2372 Soft production lock + force-deopt ABI — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
