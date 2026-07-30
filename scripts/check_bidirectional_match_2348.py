#!/usr/bin/env python3
"""Issue #2348: bidirectional check-mode for ADT match + GuardShape.

Contract:
  AC1 Match check-mode: check_flat_match under expected; pattern membership
  AC2 GuardShape: check_flat_if_narrowing refined then-branch when bidirectional
  AC3 Opt-out: bidirectional_mode=false → synthesize-only match path
  AC4 Observability: match-check / match-refined / schema-2348 keys
  AC5 Tests + source-cite + selective renarrow retained

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

    hdr = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    prim = _read("src/compiler/evaluator_primitives_compile.cpp")
    test = _read("tests/compiler/test_bidirectional_match_check_2348.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 match check-mode
    must("check_flat_match", "AC1", hdr)
    must("check_flat_match", "AC1", impl)
    must("__match_tmp", "AC1", impl)
    must("bidirectional_match_check_total", "AC1", met)
    must("bidirectional_match_refined_total", "AC1", met)
    must("ac1_match_check_mode", "AC1", test)

    # AC2 GuardShape
    must("bidirectional_guardshape_check_total", "AC2", met)
    must("bidirectional_guardshape_check_total", "AC2", impl)
    must("check_flat_if_narrowing", "AC2", impl)
    must("selective_adt_guardshape_renarrow", "AC2", hdr)
    must("ac2_guardshape_check", "AC2", test)

    # AC3 opt-out
    must("bidirectional_mode_", "AC3", impl)
    must("ac3_opt_out", "AC3", test)
    must("set_bidirectional_mode(false)", "AC3", test)

    # AC4 observability
    must("schema-2348", "AC4", prim)
    must("issue-2348", "AC4", prim)
    must("match-check", "AC4", prim)
    must("match-refined", "AC4", prim)
    must("guardshape-check", "AC4", prim)
    must("match-check-wired", "AC4", prim)
    must("ac4_observability", "AC4", test)

    # AC5 tests + gate
    must("ac5_source_cite", "AC5", test)
    must("test_bidirectional_match_check_2348", "AC5", cmake)
    must("check_bidirectional_match_2348", "AC5", build)
    must("cmd_bidirectional_match_coverage", "AC5", build)
    must("Issue #2348", "AC5", impl)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2348 bidirectional match check-mode — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
