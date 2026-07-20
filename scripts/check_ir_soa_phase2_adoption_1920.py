#!/usr/bin/env python3
"""check_ir_soa_phase2_adoption_1920.py — Issue #1920 source gate.

  AC1: kIrSoaMigrationPhase == 2 + consumer counters
  AC2: IRModuleV2View / walk_soa_function_hotpath / to_aos_module
  AC3: DCE/TypeProp/ConstFold run(IRModuleV2)
  AC4: dirty-driven skip/run recording
  AC5: capture dirty mark in service mark_define_dirty
  AC6: lowering records consumer_lowering when dual-emit
  AC7: soa-adoption-stats schema-1920
  AC8: test exists
  AC9: consult_shape/linear record column consults
  AC10: phase2-consumer-wired flag

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATS = ROOT / "src" / "compiler" / "jit_typed_mutation_stats.h"
SOA = ROOT / "src" / "compiler" / "soa_view.ixx"
IRSOA = ROOT / "src" / "compiler" / "ir_soa.ixx"
PM = ROOT / "src" / "compiler" / "pass_manager.ixx"
LOW = ROOT / "src" / "compiler" / "lowering_impl.cpp"
SVC = ROOT / "src" / "compiler" / "service.ixx"
OBS = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "test_ir_soa_phase2_adoption_1920.cpp"


def main() -> int:
    failures: list[str] = []
    stats = STATS.read_text(encoding="utf-8", errors="replace")
    soa = SOA.read_text(encoding="utf-8", errors="replace")
    irsoa = IRSOA.read_text(encoding="utf-8", errors="replace")
    pm = PM.read_text(encoding="utf-8", errors="replace")
    low = LOW.read_text(encoding="utf-8", errors="replace")
    svc = SVC.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")

    if "kIrSoaMigrationPhase = 2" not in stats and "kIrSoaMigrationPhase=2" not in stats:
        failures.append("AC1: phase != 2")
    for c in (
        "consumer_lowering_hits",
        "consumer_executor_hits",
        "consumer_pass_hits",
        "consumer_jit_hits",
    ):
        if c not in stats:
            failures.append(f"AC1: missing {c}")

    if "IRModuleV2View" not in soa:
        failures.append("AC2: IRModuleV2View missing")
    if "walk_soa_function_hotpath" not in irsoa:
        failures.append("AC2: walk_soa_function_hotpath missing")
    if "to_aos_module" not in irsoa:
        failures.append("AC2: to_aos_module missing")

    if "void run(IRModuleV2& mod, bool dirty_blocks_only" not in pm:
        failures.append("AC3: SoA run overloads missing")
    if pm.count("record_consumer_pass") < 2:
        failures.append("AC3: too few record_consumer_pass")

    if "record_dirty_block_skip" not in pm and "record_dirty_block_skip" not in stats:
        failures.append("AC4: dirty skip missing")

    if "record_capture_dirty_mark" not in svc:
        failures.append("AC5: capture dirty not wired in service")

    if "record_consumer_lowering" not in low:
        failures.append("AC6: lowering consumer not wired")

    if "schema-1920" not in obs or "consumer-families-active" not in obs:
        failures.append("AC7: schema-1920 / families missing from soa-adoption-stats")

    if not TEST.is_file():
        failures.append("AC8: test missing")

    if "record_shape_column_consult" not in soa:
        failures.append("AC9: shape consult not recording")

    if "phase2-consumer-wired" not in obs:
        failures.append("AC10: phase2-consumer-wired missing")

    if failures:
        print("check_ir_soa_phase2_adoption_1920: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_ir_soa_phase2_adoption_1920: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
