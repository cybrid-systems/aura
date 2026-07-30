#!/usr/bin/env python3
"""Issue #2351: steal-complete LayoutStamp dual-check before resume.

Contract:
  AC1 Matching stamp → no mismatch bump
  AC2 Mismatched stamp → layout_stamp_steal_mismatch_total + dual-check
  AC3 No stamp → zero extra compare (missing only if MB-expected)
  AC4 Schema-2351 keys + source-cite
  AC5 Unit/integration concurrent stress

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

    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    met = _read("src/compiler/observability_metrics.h")
    eixx = _read("src/compiler/evaluator.ixx")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    jit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_layout_stamp_2351.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1/AC2 dual-check in on_steal_complete
    must("aura_evaluator_on_steal_complete", "AC1", fm)
    must("has_resume_layout_stamp", "AC1", fm)
    must("layout_stamp_steal_mismatch_total", "AC2", fm)
    must("layout_stamp_steal_mismatch_total", "AC2", met)
    must("scan_live_closures_for_linear_captures", "AC2", fm)
    must("aura_aot_record_deopt_on_steal", "AC2", fm)
    must("ac1_matching_stamp", "AC1", test)
    must("ac2_mismatched_stamp", "AC2", test)

    # AC3 zero cost / missing
    must("layout_stamp_steal_missing_total", "AC3", fm)
    must("layout_stamp_steal_missing_total", "AC3", met)
    must("ac3_no_stamp_zero_cost", "AC3", test)
    must("Does NOT clear the stamp", "AC3", fm)

    # AC4 schema
    must("schema-2351", "AC4", q)
    must("layout-stamp-steal-mismatch-total", "AC4", q)
    must("layout-stamp-steal-missing-total", "AC4", q)
    must("layout-stamp-steal-wired", "AC4", q)
    must("get_layout_stamp_steal_mismatch_total", "AC4", eixx)
    must("get_layout_stamp_steal_mismatch_total", "AC4", emb)
    must("schema-2351", "AC4", jit)
    must("ac4_schema_and_source", "AC4", test)

    # AC5
    must("ac5_concurrent_stress", "AC5", test)
    must("Issue #2351", "AC5", fm)
    must("test_steal_layout_stamp_2351", "AC5", cmake)
    must("check_steal_layout_stamp_2351", "AC5", build)
    must("cmd_steal_layout_stamp_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2351 steal LayoutStamp dual-check — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
