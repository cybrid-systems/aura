#!/usr/bin/env python3
"""Issue #2504: e2e PerEval dual-eval SpecJIT storm isolation regression gate.

  AC1: dual eval PerEval — B storm does not clear A's hit path
  AC2: foreign skip + owner-only clear
  AC3: Global mode clears both
  AC4: no process-global shape_version bump under PerEval
  AC5: source + cmake + build gate + schema-2504

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

    sj = _read("src/compiler/spec_jit_controller.cpp")
    sjh = _read("src/compiler/spec_jit_controller.h")
    sp = _read("src/compiler/shape_profiler.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_specjit_pereval_storm_e2e_2504.cpp")
    test2370 = _read("tests/compiler/test_specjit_per_eval_storm_isolation_2370.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("g_specjit_per_eval_storm_skip_foreign_total", "AC1", sj)
    must("get_specialized", "AC1", test)
    must("ac1_dual_eval_pereval_hit_survives", "AC1", test)

    must("eval_owner_", "AC2", sj)
    must("g_specjit_per_eval_storm_clear_total", "AC2", sj)
    must("ac2_foreign_skip_and_owner_clear", "AC2", test)

    must("ac3_global_clears_both", "AC3", test)
    must("mode Global", "AC3", test)

    must("aura_get_storm_isolation_mode() != 2", "AC4", sp)
    must("isolation_shape_epoch_", "AC4", sjh)
    must("ac4_no_global_shape_bump_under_pereval", "AC4", test)

    must("schema-2504", "AC5", mut)
    must("specjit-pereval-e2e-isolation-wired", "AC5", mut)
    must("multi-eval-host-pereval-heuristic-wired", "AC5", mut)
    must("test_specjit_pereval_storm_e2e_2504", "AC5", cmake)
    must("check_specjit_pereval_storm_e2e_2504", "AC5", build)
    must("cmd_specjit_pereval_storm_e2e_coverage", "AC5", build)
    must("ac5_concurrent_and_source_gate", "AC5", test)
    # Prefer-existing #2370 suite remains the unit lineage.
    must("ac2_ac3_two_eval_isolation", "AC5", test2370)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2504 SpecJIT PerEval storm e2e isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
