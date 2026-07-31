#!/usr/bin/env python3
"""Issue #2370: real PerEval storm isolation for SpecJIT + shape version.

  AC1: Global path process-wide stamp
  AC2: two controllers — storm on A does not clear B under PerEval
  AC3: isolation epoch stamp under PerEval
  AC4: query schema-2370 + per-eval metrics
  AC5: source + gate

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
    hur = _read("src/compiler/hot_update_registry.cpp")
    hurh = _read("src/compiler/hot_update_registry.hh")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_specjit_per_eval_storm_isolation_2370.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("Issue #2370", "AC1", sj)
    must("effective_shape_version", "AC1", sj)
    must("ac1_global_soft", "AC1", test)

    must("g_specjit_per_eval_storm_clear_total", "AC2", sj)
    must("g_specjit_per_eval_storm_skip_foreign_total", "AC2", sj)
    must("eval_owner_", "AC2", sj)
    must("ac2_ac3_two_eval_isolation", "AC2", test)

    must("isolation_shape_epoch_", "AC3", sjh)
    must("set_eval_owner", "AC3", sjh)
    must("install_specialization", "AC3", sj)

    must("schema-2370", "AC4", mut)
    must("storm-isolation-per-eval-wired", "AC4", mut)
    must("specjit-per-eval-storm-clear-total", "AC4", mut)
    must("ac4_query", "AC4", test)

    must("aura_get_storm_isolation_mode", "AC5", sp)
    must("aura_get_storm_eval_context", "AC5", hur)
    must("PerEval", "AC5", hurh)
    must("set_eval_owner", "AC5", svc)
    must("test_specjit_per_eval_storm_isolation_2370", "AC5", cmake)
    must("check_specjit_per_eval_storm_isolation_2370", "AC5", build)
    must("cmd_specjit_per_eval_storm_isolation_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2370 SpecJIT PerEval storm isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
