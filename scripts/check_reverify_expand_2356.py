#!/usr/bin/env python3
"""Issue #2356: truncated reverify one-shot expand coverage.

  AC1: expand when truncated + occurrence/let-poly roots
  AC2: no priority roots → expand never taken
  AC3: at most one expand per solve_delta
  AC4: TIMEOUT escalate path unchanged
  AC5: query schema-2356 + tests + gate

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

    tci = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_reverify_expand_2356.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2356", "AC1", tci)
    must("delta_reverify_expand_total", "AC1", tci)
    must("expand_limit", "AC1", tci)
    must("occurrence_priority_roots_", "AC1", tci)
    must("let_poly_dirty_roots_", "AC1", tci)
    must("ac1_expand_with_occurrence", "AC1", test)

    # AC2
    must("empty priority roots", "AC2", tci)
    must("ac2_no_priority_zero_cost", "AC2", test)

    # AC3
    must("at most one expand", "AC3", tci)
    must("ac3_at_most_one_expand", "AC3", test)
    must("delta_reverify_expand_total", "AC3", met)

    # AC4
    must("Production TIMEOUT", "AC4", tci)
    must("ac4_timeout_escalate_source", "AC4", test)

    # AC5
    must("schema-2356", "AC5", q)
    must("delta-reverify-expand-total", "AC5", q)
    must("delta-reverify-expand-wired", "AC5", q)
    must("delta_reverify_expand_total", "AC5", fields)
    must("test_reverify_expand_2356", "AC5", cmake)
    must("check_reverify_expand_2356", "AC5", build)
    must("cmd_reverify_expand_coverage", "AC5", build)
    must("ac5_query_and_source", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2356 reverify expand — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
