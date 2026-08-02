#!/usr/bin/env python3
"""Issue #2563: cross-closure linear escape discovery coverage.

Contract:
  AC1 discover_cross_closure_linear_escapes + hard/Soft force path
  AC2 zero work without capture; CrossBatch/PostMutate retained
  AC3 #2545 force_linear_rollback single entry + CrossClosureEscape authority
  AC4 schema-2563 + query keys + source-cite
  AC5 cone cap (#2560 soft) + cap_trunc counter; test + cmake + gate

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

    etc = _read("src/compiler/evaluator_typecheck.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    tci = _read("src/compiler/type_checker.ixx")
    tcpp = _read("src/compiler/type_checker_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/compiler/test_linear_cross_closure_escape_2563.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("discover_cross_closure_linear_escapes", "AC1", tci)
    must("discover_cross_closure_linear_escapes", "AC1", tcpp)
    must("discover_cross_closure_linear_escapes", "AC1", etc)
    must("CrossClosureEscape", "AC1", eixx)
    must("linear-cross-closure-escape", "AC1", etc)
    must("linear_cross_closure_force_total", "AC1", aud)
    must("linear_cross_closure_observe_total", "AC1", aud)
    must("ac1_discover_hard_soft", "AC1", test)

    # AC2
    must("CrossBatchEscape", "AC2", etc)
    must("PostMutateLinear", "AC2", etc)
    must("ac2_zero_and_existing", "AC2", test)

    # AC3
    must("force_linear_rollback", "AC3", emb)
    must("force_linear_rollback", "AC3", etc)
    must("case LinearForceAuthority::CrossClosureEscape", "AC3", etc)
    must("ac3_unified_entry", "AC3", test)

    # AC4
    must("schema-2563", "AC4", q)
    must("linear-cross-closure-escape-total", "AC4", q)
    must("linear_cross_closure_hard_enabled", "AC4", aud)
    must("cross_closure_linear_escape", "AC4", aud)
    must("ac4_schema", "AC4", test)

    # AC5
    must("partial_cone_soft_cap_for_linear", "AC5", etc)
    must("linear_cross_closure_cap_trunc_total", "AC5", aud)
    must("cap_truncations", "AC5", tcpp)
    must("test_linear_cross_closure_escape_2563", "AC5", cmake)
    must("check_linear_cross_closure_escape_2563", "AC5", build)
    must("cmd_linear_cross_closure_escape_coverage", "AC5", build)
    must("ac5_cone_cap", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2563 cross-closure linear escape — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
