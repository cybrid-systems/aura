#!/usr/bin/env python3
"""Issue #2560: partial re-infer cone soft/hard cap coverage.

Contract:
  AC1 soft cap + soft_overflow + seed-preserving truncate
  AC2 hard fallback under production_defaults_active
  AC3 under soft zero extra overflow path
  AC4 #2516 order preserved (cap before invalidate)
  AC5 schema-2560 + metrics + test + cmake + build.py

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    mh = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_partial_cone_cap_2560.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2560", "AC1", impl)
    must("partial_cone_soft_cap", "AC1", impl)
    must("partial_cone_soft_overflow_total", "AC1", impl)
    must("AURA_PARTIAL_CONE_SOFT", "AC1", impl)
    must("truncate_partial_cone_seed_preserving", "AC1", impl)
    must("type_dep_fanout_cap", "AC1", impl)
    must("ac1_soft_overflow_path", "AC1", test)

    # AC2
    must("partial_cone_hard_fallback_total", "AC2", impl)
    must("AURA_PARTIAL_CONE_HARD", "AC2", impl)
    must("production_defaults_active", "AC2", impl)
    must("orig_sz > hard", "AC2", impl)
    must("ac2_hard_production", "AC2", test)

    # AC3
    must("orig_sz > soft", "AC3", impl)
    must("ac3_under_soft_zero", "AC3", test)

    # AC4
    must("Issue #2516 dirty txn", "AC4", impl)
    must("Issue #2560", "AC4", ixx)
    must("AURA_PARTIAL_CONE_SOFT", "AC4", ixx)
    must("ac4_txn_order", "AC4", test)
    # Cap before #2516 block
    cap_pos = impl.find("Issue #2560: partial cone soft/hard SLA")
    txn_pos = impl.find("Issue #2516 dirty txn")
    if cap_pos < 0 or txn_pos < 0 or cap_pos > txn_pos:
        fails.append("AC4: cone cap block must precede #2516 dirty txn")

    # AC5
    must("partial_cone_soft_overflow_total", "AC5", mh)
    must("partial_cone_hard_fallback_total", "AC5", mh)
    must("partial_cone_type_dep_degree_trunc_total", "AC5", mh)
    must("partial_cone_last_size", "AC5", mh)
    must("schema-2560", "AC5", q)
    must("partial-cone-soft-overflow-total", "AC5", q)
    must("partial-cone-hard-fallback-total", "AC5", q)
    must("partial-cone-last-size", "AC5", q)
    must("test_partial_cone_cap_2560", "AC5", cmake)
    must("check_partial_cone_cap_2560", "AC5", build)
    must("cmd_partial_cone_cap_coverage", "AC5", build)
    must("ac5_schema", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2560 partial cone soft/hard cap — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
