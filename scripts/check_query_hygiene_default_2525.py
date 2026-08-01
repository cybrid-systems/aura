#!/usr/bin/env python3
"""Issue #2525: unconstrained query hygiene residual — default MacroIntroduced skip.

Contract:
  AC1 default pattern + filter skip MacroIntroduced; include restores
  AC2 composite index rebuild stamps marker dimension
  AC3 hygiene_skip_total / hygiene_include_total + schema-2525
  AC4 concurrent path retained
  AC5 prefer-existing #2123 + residual test
  AC6 Agent contract docs; schema-2123 no break

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

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    idx = _read("src/compiler/evaluator_query_index.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_query_hygiene_default_2525.cpp")
    t2123 = _read("tests/compiler/test_query_pattern_default_hygiene_2123.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("#2525", "AC1", qws)
    must("hygiene_skip_macro = true", "AC1", qws)
    must(":include-macro-introduced", "AC1", qws)
    must("ac1_default_skip", "AC1", test)

    # AC2
    must("#2525", "AC2", idx)
    must("tag_arity_index_user_", "AC2", idx)
    must("tag_arity_marker_dimension_rebuild_total", "AC2", idx)
    must("ac2_index_marker_dimension", "AC2", test)

    # AC3
    must("hygiene_skip_total", "AC3", met)
    must("hygiene_include_total", "AC3", met)
    must("pattern_hygiene_unconstrained_walk_total", "AC3", met)
    must("hygiene_skip_total", "AC3", fields)
    must("schema-2525", "AC3", q)
    must("hygiene_skip_total", "AC3", q)
    must("hygiene_include_total", "AC3", q)
    must("ac3_stats_surface", "AC3", test)

    # AC4
    must("ac4_concurrent", "AC4", test)

    # AC5
    must("#2123", "AC5", t2123)
    must("ac5_ac6_lineage_and_contract", "AC5", test)

    # AC6
    must("schema-2123", "AC6", q)
    must("Agent contract", "AC6", qws)
    must("test_query_hygiene_default_2525", "AC6", cmake)
    must("check_query_hygiene_default_2525", "AC6", build)
    must("cmd_query_hygiene_default_coverage", "AC6", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2525 query hygiene residual default — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
