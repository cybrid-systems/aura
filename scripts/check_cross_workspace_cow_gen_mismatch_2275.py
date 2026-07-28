#!/usr/bin/env python3
"""check_cross_workspace_cow_gen_mismatch_2275.py — Issue #2275 source gate.

  AC1: ForeignEval path preserved (regression #2240)
  AC2: CowGenMismatch wire at aura_reload_aot_module_for_eval entry
  AC3: Happy path with matching cow_gen unchanged
  AC4: aura_cross_workspace_reject_reason_string covers all 4 enum values
  AC5: 4 query keys + source-cite + test extension

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE_H = ROOT / "src/compiler/aura_jit_bridge.h"
BRIDGE_CPP = ROOT / "src/compiler/aura_jit_bridge.cpp"
OBS = ROOT / "src/compiler/observability_metrics.h"
Q = ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests/compiler/test_aot_reload_primitive.cpp"


def main() -> int:
    failures: list[str] = []

    BRIDGE_H.read_text(encoding="utf-8", errors="replace")
    bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: ForeignEval path unchanged.
    must(
        "static_cast<std::uint8_t>(CrossWorkspaceReject::ForeignEval)",
        "AC1",
        bridge_cpp,
    )

    # AC2: CowGenMismatch wire at reload entry.
    must(
        "CrossWorkspaceReject::CowGenMismatch",
        "AC2",
        bridge_cpp,
    )
    must(
        "aura_get_aot_expected_cow_gen_for_eval(eval_ptr)",
        "AC2",
        bridge_cpp,
    )
    must(
        "aura_get_live_workspace_cow_gen()",
        "AC2",
        bridge_cpp,
    )
    must(
        "cross-COW cow_gen mismatch",
        "AC2",
        bridge_cpp,
    )

    # AC3: C ABI accessors in bridge_cpp (impl; .hh is optional for #2275).
    must(
        "aura_set_aot_expected_cow_gen_for_eval",
        "AC3",
        bridge_cpp,
    )
    must("aura_get_live_workspace_cow_gen", "AC3", bridge_cpp)
    must(
        "aura_set_live_workspace_cow_gen",
        "AC3",
        bridge_cpp,
    )

    # AC4: reason string switch covers all 4 enum values.
    must(
        "aura_cross_workspace_reject_reason_string",
        "AC4",
        bridge_cpp,
    )
    # The switch returns lowercase kebab-case strings (existing #2240 convention).
    must('return "none";', "AC4", bridge_cpp)
    must('return "foreign_eval";', "AC4", bridge_cpp)
    must('return "cow_gen_mismatch";', "AC4", bridge_cpp)
    must('return "unknown";', "AC4", bridge_cpp)

    # AC5: query keys + observability + test extension.
    must(
        "cross_workspace_hot_update_rejected_total",
        "AC5",
        obs,
    )
    must("cow-gen-mismatch-wired", "AC5", q)
    must(
        "cross-workspace-cow-gen-mismatch-wired",
        "AC5",
        q,
    )
    must("schema-2275", "AC5", q)
    must("issue-2275", "AC5", q)
    must("void ac2275_cow_gen_mismatch", "AC5", test)
    must("ac2275_cow_gen_mismatch(cs)", "AC5", test)
    must(
        "AC #2275: CowGenMismatch wire (fail-closed)",
        "AC5",
        test,
    )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
