#!/usr/bin/env python3
"""Issue #2371: cross-COW dual-epoch soft restamp vs hard-reject.

  AC1: soft migrate helper + default-on policy
  AC2: hard reject for unsafe / disabled soft
  AC3: max drift cap
  AC4: query schema-2371 + metrics
  AC5: tests + gate

Exit 0 = all ACs satisfied.
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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_cross_cow_soft_migrate_2371.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("Issue #2371", "AC1", rt)
    must("try_cross_cow_soft_migrate_", "AC1", rt)
    must("cross_cow_soft_migrate_enabled_", "AC1", rt)
    must("AURA_CROSS_COW_SOFT_MIGRATE", "AC1", rt)
    must("stamp_closure_provenance_locked", "AC1", rt)
    must("ac1_soft_migrate", "AC1", test)

    must("cross_cow_hard_reject_total", "AC2", rt)
    must("cross_cow_hard_reject_total", "AC2", obs)
    must("ac2_hard_reject", "AC2", test)

    must("cross_cow_soft_migrate_max_drift_", "AC3", rt)
    must("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "AC3", rt)
    must("ac3_far_behind", "AC3", test)

    must("schema-2371", "AC4", q)
    must("cross-cow-soft-migrate-wired", "AC4", q)
    must("cross-cow-soft-migrate-total", "AC4", q)
    must("cross-cow-hard-reject-total", "AC4", q)
    must("ac4_query", "AC4", test)

    must("test_cross_cow_soft_migrate_2371", "AC5", cmake)
    must("check_cross_cow_soft_migrate_2371", "AC5", build)
    must("cmd_cross_cow_soft_migrate_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2371 cross-COW soft migrate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
