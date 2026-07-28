#!/usr/bin/env python3
"""check_closure_capture_env_gen_remount_2272.py — Issue #2272 source gate.

  AC1: C ABI aura_closure_set_env_gen / aura_closure_get_env_gen + g_closure_env_gen
      vector + stamp_closure_provenance_locked env_gen path
  AC2: aura_remount_closure_captures PRIMARY env_gen check + mismatch counter
  AC3: aura_closure_has_env_or_linear_captures includes env_gen as env capture
  AC4: closure_capture_env_gen_mismatch_total counter + 4 new query keys +
      schema-2272/issue-2272 lineage
  AC5: Test extension (tests/compiler/test_aot_incremental_reemit.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JIT_RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRIDGE_CPP = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
OBS = ROOT / "src" / "compiler" / "observability_metrics.h"
EV = ROOT / "src" / "compiler" / "evaluator.ixx"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp"
TEST = ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp"


def main() -> int:
    failures: list[str] = []

    jit_rt = JIT_RT.read_text(encoding="utf-8", errors="replace")
    bridge_h = BRIDGE_H.read_text(encoding="utf-8", errors="replace")
    bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    ev = EV.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: C ABI declared + impls + vector + stamp path.
    must("aura_closure_set_env_gen", "AC1", bridge_h)
    must("aura_closure_get_env_gen", "AC1", bridge_h)
    must('extern "C" void aura_closure_set_env_gen', "AC1", jit_rt)
    must('extern "C" std::uint64_t aura_closure_get_env_gen', "AC1", jit_rt)
    must(
        "static std::vector<std::uint64_t> g_closure_env_gen",
        "AC1",
        jit_rt,
    )
    must("stamp_closure_provenance_locked", "AC1", jit_rt)

    # AC2: PRIMARY env_gen check + mismatch counter.
    must("cid_env_gen != 0 && cid_env_gen != live_env_gen", "AC2", jit_rt)
    must(
        "aura_bump_closure_capture_env_gen_mismatch_total",
        "AC2",
        bridge_cpp,
    )

    # AC3: env_gen counted as env capture in has-env-or-linear.
    must(
        "has_env_gen = cid < g_closure_env_gen.size()",
        "AC3",
        jit_rt,
    )

    # AC4: counter + query keys + schema-2272 lineage.
    must("closure_capture_env_gen_mismatch_total{0}", "AC4", obs)
    must(
        "get_closure_capture_env_gen_mismatch_total",
        "AC4",
        ev,
    )
    must("closure-capture-env-gen-mismatch-total", "AC4", q)
    must("closure-capture-env-gen-wired", "AC4", q)
    must("schema-2272", "AC4", q)
    must("issue-2272", "AC4", q)

    # AC5: test extension.
    must("ac2272_env_gen_remount", "AC5", test)
    must("ac2272_env_gen_remount(cs)", "AC5", test)
    must(
        "AC #2272: closure remount env_generation_ PRIMARY axis",
        "AC5",
        test,
    )
    must(
        "AC5: closure_capture_env_gen_mismatch_total bumped",
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
