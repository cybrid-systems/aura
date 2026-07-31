#!/usr/bin/env python3
"""Issue #2432: IR SoA generation fence on LayoutStamp (8th field).

Contract:
  AC1 LayoutStamp.ir_soa_generation + fiber resume compare
  AC2 process-global g_ir_soa_generation_fence advanced on bump_generation
  AC3 ir_generation_fence_hit_total metric + query keys
  AC4 gate + test wired

Exit 0 = all rows satisfied.
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

    stamp = _read("src/core/layout_stamp.hh")
    soa = _read("src/compiler/ir_soa.ixx")
    fiber = _read("src/serve/fiber.h")
    fib_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mut_b = _read("src/compiler/evaluator_mutation_boundary.cpp")
    metrics = _read("src/compiler/observability_metrics.h")
    query = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_ir_soa_layout_stamp_2432.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2432", "AC1", stamp)
    must("ir_soa_generation", "AC1", stamp)
    must("kLayoutStampSchema = 2432", "AC1", stamp)
    must("resume_ir_soa_generation_", "AC1", fiber)
    must("resume_ir_soa_generation()", "AC1", fiber)
    must("resume_ir_soa_generation() != cur.ir_soa_generation", "AC1", fib_mut)
    must("2432 AC1", "AC1", test)

    must("g_ir_soa_generation_fence", "AC2", soa)
    must("current_ir_soa_generation_fence", "AC2", soa)
    must("g_ir_soa_generation_fence().fetch_add", "AC2", soa)
    must("2432 AC3", "AC3", test)

    must("ir_generation_fence_hit_total", "AC3", metrics)
    must("ir_generation_fence_hit_total", "AC3", fib_mut)
    must("stamp.ir_soa_generation", "AC3", mut_b)
    must("ir-generation-fence-hit-total", "AC3", query)
    must("schema-2432", "AC3", query)
    must("2432 AC4", "AC4", test)

    must("check_ir_soa_layout_stamp_2432", "gate", build)
    must("cmd_ir_soa_layout_stamp_coverage", "gate", build)
    must("test_ir_soa_layout_stamp_2432", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: ir soa layout stamp #2432 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
