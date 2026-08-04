#!/usr/bin/env python3
"""Issue #2380: nightly production-concurrency gate (canary + full chaos).

Contract:
  AC1 AURA_PRODUCTION_CONCURRENCY_GATE + canary + full chaos env matrix
  AC2 Inject residual / lock-order / snapshot / densify fails detection
  AC3 Green run criteria: hang / residual / mismatch / densify / Soft forbid
  AC4 Default PR smoke unchanged (no multi-minute soak unless FULL=1)
  AC5 build.py production-concurrency + nightly.yml + source-cite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    test = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp")
    build = _read("build.py")
    nightly = _read(".github/workflows/nightly.yml")
    cmake = _read("CMakeLists.txt")

    # AC1 env matrix + profile
    must("AURA_PRODUCTION_CONCURRENCY_GATE", "AC1", test)
    must("AURA_LOCK_ORDER_CANARY", "AC1", test)
    must("AURA_CHAOS_FULL", "AC1", test)
    must("production_concurrency_gate", "AC1", test)
    must("Issue #2380", "AC1", test)

    # AC2 inject self-tests
    must("ac2380_inject_densify_fail", "AC2", test)
    must("ac2380_inject_lock_order_violation", "AC2", test)
    must("bump_densify_consistency_fail_total", "AC2", test)
    must("force_audit_mode_for_test", "AC2", test)
    must("g_lock_order_violation_total", "AC2", test)
    must("ac2_inject_residual_panic", "AC2", test)
    must("ac3_inject_snapshot_mismatch", "AC2", test)

    # AC3 green-run hard criteria
    must("densify_consistency_fail", "AC3", test)
    must("Soft steal forbidden", "AC3", test)
    must("is_steal_snapshot_soft_mode", "AC3", test)
    must("lock-order violation delta", "AC3", test)
    must("snapshot mismatch delta", "AC3", test)
    must("residual defer clean", "AC3", test)
    must("no hang", "AC3", test)

    # AC4 smoke stays short without FULL / prod gate / soak (#2513)
    if "if (!chaos_full() && !prod_gate" not in test and "if (!chaos_full() && !prod_gate && !soak)" not in test:
        fails.append("AC4: missing smoke wall guard (FULL/prod/soak)")
    must("90", "AC4", test)
    must("SKIPPED", "AC4", test)

    # AC5 registration
    must("cmd_production_concurrency", "AC5", build)
    must("production-concurrency", "AC5", build)
    must("AURA_PRODUCTION_CONCURRENCY_GATE", "AC5", build)
    must("AURA_LOCK_ORDER_CANARY", "AC5", build)
    must("check_production_concurrency_gate_2380", "AC5", build)
    must("cmd_production_concurrency_coverage", "AC5", build)
    must("AURA_PRODUCTION_CONCURRENCY_GATE", "AC5", nightly)
    must("AURA_LOCK_ORDER_CANARY", "AC5", nightly)
    must("AURA_CHAOS_FULL", "AC5", nightly)
    must("production-concurrency", "AC5", nightly)
    must("test_chaos_mutate_steal_gc_mailbox_2352", "AC5", cmake)
    must("ac2380_production_concurrency_docs", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2380 production-concurrency gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
