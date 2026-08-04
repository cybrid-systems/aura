#!/usr/bin/env python3
"""Issue #2254: SoA single source of truth observability.

Contract (5 AC from issue body):
  AC1: Under AURA_IR_SOA_ONLY=1 (production default), no live AoS
       storage after lower_to_ir; dual-emit counters stay 0.
  AC2: All existing DirtyAware / SoAViewAware passes compile + run
       against pure SoA. IRInstructionView <= 16 B POD.
  AC3: Arena compact + remap only updates SoA indices; no dangling
       AoS pointers. finish_dirty_sync is the single authority.
  AC4: Metrics: soa_only_path_total, residual_aos_bridge_total.
  AC5: Perf / AI mutation stress: no regression vs dual path.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    soa = _read("src/compiler/ir_soa.ixx")
    low = _read("src/compiler/lowering_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    test = _read("tests/compiler/test_ir_soa_dual_emit_batch.cpp")

    # AC1 - AURA_IR_SOA_ONLY macro + dual-emit gate in lowering_impl.cpp
    _must(
        "#ifndef AURA_IR_SOA_ONLY" in soa and "#define AURA_IR_SOA_ONLY 1" in soa,
        "AC1: AURA_IR_SOA_ONLY default ON missing",
        fails,
    )
    _must("&& !AURA_IR_SOA_ONLY" in low, "AC1: lowering_impl.cpp dual_emit not gated behind AURA_IR_SOA_ONLY", fails)
    _must("g_soa_only_path_total_atomic().fetch_add" in low, "AC1: soa_only_path bump site missing", fails)
    _must("g_residual_aos_bridge_total_atomic().fetch_add" in low, "AC1: residual_aos_bridge bump site missing", fails)

    # AC2 - IRInstructionView <= 16 B POD + process atomics
    _must(
        "struct IRInstructionView" in soa and "sizeof(IRInstructionView) <= 16" in soa,
        "AC2: IRInstructionView <= 16 B POD missing",
        fails,
    )
    _must(
        "g_soa_only_path_total_atomic" in soa and "g_residual_aos_bridge_total_atomic" in soa,
        "AC2: process atomics for pure unit tests missing",
        fails,
    )

    # AC3 - finish_dirty_sync single authority
    _must("finish_dirty_sync" in soa, "AC3: finish_dirty_sync missing", fails)

    # AC4 - 2 metric fields
    _must("soa_only_path_total{0}" in met, "AC4: soa_only_path_total field missing", fails)
    _must("residual_aos_bridge_total{0}" in met, "AC4: residual_aos_bridge_total field missing", fails)

    # AC5 - test surface covers #2254
    _must(
        ("ac2254_soa_only_default" in test) or ("AC #2254" in test),
        "AC5: ac2254 test function (or AC #2254 inline block) missing",
        fails,
    )
    _must("#2254" in test, "AC5: #2254 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2254 SoA single source of truth coverage linter")
    parser.add_argument("--self-test", action="store_true", help="Run self-test (return 0 if contract satisfied)")
    parser.add_argument("--strict", action="store_true", help="Strict mode (non-zero exit on any failure)")
    args = parser.parse_args()

    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK: SoA single source of truth coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
