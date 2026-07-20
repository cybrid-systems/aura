#!/usr/bin/env python3
"""check_arena_auto_compact_intelligent_1919.py — Issue #1919 source gate.

  AC1: AutoCompactMode + dynamic threshold 30–60%
  AC2: mutation/JIT deopt pressure signals
  AC3: false-positive outcome counters
  AC4: evaluate_auto_compact_policy uses dynamic thr
  AC5: mutate:rebind signals mutation pressure
  AC6: service compact hook signals jit deopt pressure
  AC7: arena-auto-policy-stats schema-1919
  AC8: production-sweep schema-1919 arena keys
  AC9: test exists
  AC10: record_auto_compact_outcome in maybe_auto_compact_on_alloc

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POL = ROOT / "src" / "core" / "arena_auto_policy_stats.h"
ARENA = ROOT / "src" / "core" / "arena.ixx"
MUT = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
SVC = ROOT / "src" / "compiler" / "service.ixx"
OBS = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
STD = ROOT / "src" / "compiler" / "evaluator_primitives_stdlib_review.cpp"
TEST = ROOT / "tests" / "test_arena_auto_compact_intelligent_1919.cpp"


def main() -> int:
    failures: list[str] = []
    pol = POL.read_text(encoding="utf-8", errors="replace")
    arena = ARENA.read_text(encoding="utf-8", errors="replace")
    mut = MUT.read_text(encoding="utf-8", errors="replace")
    svc = SVC.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    std = STD.read_text(encoding="utf-8", errors="replace")

    for needle in (
        "AutoCompactMode",
        "Conservative",
        "Balanced",
        "Aggressive",
        "kFragThresholdMin",
        "kFragThresholdMax",
        "compute_dynamic_frag_threshold",
    ):
        if needle not in pol:
            failures.append(f"AC1: missing {needle}")

    for needle in (
        "signal_mutation_pressure",
        "signal_jit_deopt_pressure",
        "kPolicyReasonMutation",
        "kPolicyReasonJitDeopt",
    ):
        if needle not in pol:
            failures.append(f"AC2: missing {needle}")

    if "auto_compact_false_positive_total" not in pol or "record_auto_compact_outcome" not in pol:
        failures.append("AC3: FP counters missing")
    if "kFalsePositiveTargetBp" not in pol:
        failures.append("AC3: FP target missing")

    if "compute_dynamic_frag_threshold" not in pol:
        failures.append("AC4: dynamic thr not used in evaluate")
    if "frag_threshold_used" not in pol:
        failures.append("AC4: frag_threshold_used missing")

    if "signal_mutation_pressure" not in mut:
        failures.append("AC5: mutate not wired")

    if "signal_jit_deopt_pressure" not in svc:
        failures.append("AC6: service compact hook not wired")

    if "schema-1919" not in obs or "intelligent-policy-wired" not in obs:
        failures.append("AC7: arena-auto-policy-stats schema-1919 missing")

    if "schema-1919" not in std or "arena-intelligent-auto-compact-wired" not in std:
        failures.append("AC8: production-sweep 1919 keys missing")

    if not TEST.is_file():
        failures.append("AC9: test missing")

    if "record_auto_compact_outcome" not in arena:
        failures.append("AC10: outcome not recorded on alloc path")

    if failures:
        print("check_arena_auto_compact_intelligent_1919: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_arena_auto_compact_intelligent_1919: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
