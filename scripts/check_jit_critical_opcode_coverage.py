#!/usr/bin/env python3
"""check_jit_critical_opcode_coverage.py — Issue #1917 source-level gate.

Verifies critical opcode lower coverage + consistency observability:

  AC1: aura_jit.h defines kCriticalOpcodeMask / kCriticalOpcodeCount=13
  AC2: critical metrics on AuraJIT::Metrics
  AC3: lower() stamps critical_opcode_lowered_total on hot opcodes
  AC4: default unhandled path bumps critical_opcode_unhandled_total
  AC5: PrimCall VectorP/ErrorP fast-path + primcall_fastpath_hits
  AC6: Apply site epoch probe (apply_site_epoch_probe_total)
  AC7: query:jit-consistency-stats exposes schema-1917 keys
  AC8: test_jit_critical_coverage_1917.cpp exists
  AC9: critical_opcode_coverage_pct helper present
  AC10: Metrics::format emits critical_* keys for stats parse

Exit 0 = all ACs satisfied, exit 1 = any failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JIT_H = ROOT / "src" / "compiler" / "aura_jit.h"
JIT_CPP = ROOT / "src" / "compiler" / "aura_jit.cpp"
QUERY_CPP = ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp"
TEST = ROOT / "tests" / "test_jit_critical_coverage_1917.cpp"


def main() -> int:
    failures: list[str] = []
    jit_h = JIT_H.read_text(encoding="utf-8", errors="replace")
    jit_cpp = JIT_CPP.read_text(encoding="utf-8", errors="replace")
    query = QUERY_CPP.read_text(encoding="utf-8", errors="replace")

    # AC1
    if "kCriticalOpcodeMask" not in jit_h:
        failures.append("AC1: kCriticalOpcodeMask missing in aura_jit.h")
    if "kCriticalOpcodeCount" not in jit_h:
        failures.append("AC1: kCriticalOpcodeCount missing")
    else:
        window = jit_h[jit_h.find("kCriticalOpcodeCount") : jit_h.find("kCriticalOpcodeCount") + 80]
        if "13" not in window:
            failures.append("AC1: kCriticalOpcodeCount != 13")

    # AC2
    for field in (
        "critical_opcode_lowered_total",
        "critical_opcode_unhandled_total",
        "primcall_fastpath_hits",
        "apply_site_epoch_probe_total",
    ):
        if field not in jit_h:
            failures.append(f"AC2: Metrics missing {field}")

    # AC3 — lowered stamps on critical ops
    if jit_cpp.count("critical_opcode_lowered_total") < 5:
        failures.append("AC3: critical_opcode_lowered_total stamped too few times in lower()")
    for op in ("OpMakeClosure", "OpApply", "OpPrimCall", "OpGuardShape", "OpCall"):
        if f"case {op}" not in jit_cpp and f"case {op}:" not in jit_cpp:
            failures.append(f"AC3: missing case {op}")

    # AC4
    if "critical_opcode_unhandled_total" not in jit_cpp:
        failures.append("AC4: critical_opcode_unhandled_total not bumped in default")

    # AC5
    if "PrimVectorP" not in jit_cpp or "PrimErrorP" not in jit_cpp:
        failures.append("AC5: PrimVectorP/PrimErrorP fast-path missing")
    if "primcall_fastpath_hits" not in jit_cpp:
        failures.append("AC5: primcall_fastpath_hits not used")

    # AC6
    if "apply_site_epoch_probe_total" not in jit_cpp:
        failures.append("AC6: apply_site_epoch_probe_total missing in Apply path")

    # AC7
    for key in (
        "schema-1917",
        "critical-opcode-coverage-pct",
        "critical-hit-rate-gate-pct",
        "make-closure-lowered-wired",
        "apply-lowered-wired",
        "primcall-fastpath-vector-error-wired",
    ):
        if key not in query:
            failures.append(f"AC7: query:jit-consistency-stats missing {key}")

    # AC8
    if not TEST.is_file():
        failures.append("AC8: tests/test_jit_critical_coverage_1917.cpp missing")

    # AC9
    if "critical_opcode_coverage_pct" not in jit_h or "critical_opcode_coverage_pct" not in jit_cpp:
        failures.append("AC9: critical_opcode_coverage_pct helper missing")

    # AC10
    if "critical_opcode_coverage_pct=" not in jit_cpp:
        failures.append("AC10: Metrics::format missing critical_opcode_coverage_pct=")
    if "primcall_fastpath_hits=" not in jit_cpp:
        failures.append("AC10: Metrics::format missing primcall_fastpath_hits=")

    if failures:
        print("check_jit_critical_opcode_coverage: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_jit_critical_opcode_coverage: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
