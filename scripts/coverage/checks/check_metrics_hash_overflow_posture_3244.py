#!/usr/bin/env python3
"""Issue #3244: production hash overflow enters security-posture / schedule-gate.

#3018/#3020 count overflow; production still had no posture / schedule signal.
This residual folds overflow into query:security-posture and observe-only
schedule force_reason (no hard admit deny yet).

Contract (one row per AC):
  AC1  production + overflow_total>0 → query:security-posture
       metrics-hash-overflow-breach + stable force_reason string
  AC2  schedule-gate surfaces force_reason; would_allow stays true;
       Soft never hard-deny / never arms
  AC3  overflow hash keeps #3018 overflow=1 plus additive hash-overflow
  AC4  capacity gate optional / not blocking this landing
  AC5  extend test_engine_metrics_facade + test_security_schedule_gate;
       no test_issue_3244.cpp; no docs/design/ (#1655)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    gate = _read("src/orch/security_schedule_gate.h")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    mbc = _read("src/compiler/evaluator_mutation_boundary.cpp")
    facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    sched = _read("tests/orch/test_security_schedule_gate.cpp")
    build = _read("build.py")

    must("kSecurityScheduleMetricsHashOverflowIssue = 3244", "AC1 stamp", gate)
    must("metrics-hash-overflow-breach", "AC1 posture key", prim)
    must("metrics-hash-overflow-breach", "AC1 force_reason", gate)
    must("production_defaults_active()", "AC1 production gate", prim)
    must("ac3244_1_prod", "AC1 test", facade)

    must("metrics_hash_overflow_would_arm", "AC2 input", gate)
    must("metrics_hash_overflow_would_arm_live", "AC2 live helper", gate)
    must("observe_metrics_hash_overflow_total", "AC2 observe counter", gate)
    must("would_allow stays true", "AC2 no admit deny comment", gate)
    must("ac3244_2_soft", "AC2 Soft test", facade)
    must("Soft never denies", "AC2 Soft schedule test", sched)
    must("metrics_hash_overflow_would_arm_live", "AC2 boundary cite", mbc)

    must("hash-overflow", "AC3 facade sentinel", obs)
    must("hash-overflow", "AC3 query sentinel", ev)
    must("overflow=1", "AC3 #3018 retained", facade)

    must("check_metrics_hash_overflow_posture_3244", "AC5 build.py", build)
    must("ac3244_1_prod", "AC5 facade test", facade)
    must("3244 AC1", "AC5 schedule test", sched)
    if _read("tests/compiler/test_issue_3244.cpp") or _read("tests/orch/test_issue_3244.cpp"):
        fails.append("AC5: test_issue_3244.cpp present (forbidden #81967)")
    if _read("docs/design/3244-metrics-hash-overflow-posture.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3244 metrics_hash_overflow_posture:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3244 metrics_hash_overflow_posture: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
