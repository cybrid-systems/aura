#!/usr/bin/env python3
"""Issue #2295: EnvFrame ownership transfer protocol coverage linter.

  AC1: EnvFrameRef::transfer_to / drop + drop bumps ownership_drop + reject
  AC2: refresh_after_fiber_migration wires transfer_to / drop on resume hint
  AC3: Happy path leaves ownership atomics at 0 (tested in unit test)
  AC4: hold_gen_mismatch surface retained
  AC5: query keys + schema-2295 lineage + tests extension

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVAL_IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EVAL_ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
EVAL_MUT = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
PRIM_Q = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
LF = ROOT / "src" / "core" / "envframe_lifetime.ixx"
TEST = ROOT / "tests" / "compiler" / "test_envframe_truncate_epoch.cpp"


def main() -> int:
    failures: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    eval_ixx = EVAL_IXX.read_text(encoding="utf-8", errors="replace")
    eval_env = EVAL_ENV.read_text(encoding="utf-8", errors="replace")
    eval_mut = EVAL_MUT.read_text(encoding="utf-8", errors="replace")
    metrics = METRICS.read_text(encoding="utf-8", errors="replace")
    prim_q = PRIM_Q.read_text(encoding="utf-8", errors="replace")
    lf = LF.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    # AC1: ownership API
    must("void transfer_to(Evaluator& ev, EnvFrameRef& dst) noexcept", "AC1", eval_ixx)
    must("void drop(Evaluator& ev) noexcept", "AC1", eval_ixx)
    must("void EnvFrameRef::transfer_to(Evaluator& ev, EnvFrameRef& dst) noexcept", "AC1", eval_env)
    must("void EnvFrameRef::drop(Evaluator& ev) noexcept", "AC1", eval_env)
    must("envframe_ownership_transfer_total", "AC1", eval_env)
    must("envframe_ownership_drop_total", "AC1", eval_env)

    # AC2: steal / refresh wire
    must("transfer_to", "AC2", eval_mut)
    must(".drop(", "AC2", eval_mut)
    must("resume_env_hint", "AC2", eval_mut)

    # AC4: hold_gen_mismatch retained
    must("hold_gen_mismatch_total", "AC4", lf)

    # AC5: metrics + query + tests
    must("envframe_ownership_transfer_total{0}", "AC5", metrics)
    must("envframe_ownership_drop_total{0}", "AC5", metrics)
    must("envframe-ownership-transfer-total", "AC5", prim_q)
    must("envframe-ownership-drop-total", "AC5", prim_q)
    must("schema-2295", "AC5", prim_q)
    must("issue-2295", "AC5", prim_q)
    must("void ac2295_ownership_transfer", "AC5", test)
    must("ac2295_ownership_transfer(cs)", "AC5", test)
    must("AC #2295: EnvFrame ownership transfer protocol", "AC5", test)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(failures)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: EnvFrame ownership transfer (#2295) — all AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
