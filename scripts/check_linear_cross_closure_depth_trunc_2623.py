#!/usr/bin/env python3
"""Issue #2623: configurable cross-closure depth + production fail-closed trunc.

Contract:
  AC1 one-level capture still works Soft observe / production force (#2563 lock)
  AC2 depth≥2 nested free-capture; production forces rollback
  AC3 cone truncation under production → trunc_force + CrossClosureEscape
  AC4 Soft + truncation → no force unless HARD; metrics only
  AC5 depth env 0 disables discovery (zero cost)
  AC6 schema-2623 additive on linear-ownership-typed-mutate-stats; source-cite
  AC7 #2559 inventory lists AURA_LINEAR_CROSS_CLOSURE_{HARD,DEPTH}

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    tci = _read("src/compiler/type_checker.ixx")
    tcpp = _read("src/compiler/type_checker_impl.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    inv = _read("scripts/check_linear_three_layer_wire_2559.py")
    test = _read("tests/compiler/test_linear_cross_closure_depth_trunc_2623.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    this = _read("scripts/check_linear_cross_closure_depth_trunc_2623.py")

    # AC1: #2563 one-level still wired
    must("discover_cross_closure_linear_escapes", "AC1", tcpp)
    must("linear_cross_closure_hard_enabled", "AC1", aud)
    must("CrossClosureEscape", "AC1", etc)
    must("ac1_one_level_soft_prod", "AC1", test)

    # AC2: nested depth ≥2
    must("production_defaults_active", "AC2", aud)
    must("return 2", "AC2", aud)  # prod default depth
    must("depth_remaining", "AC2", tcpp)
    must("depth2_entries", "AC2", tcpp)
    must("ac2_nested_depth2_prod", "AC2", test)

    # AC3: trunc fail-closed under hard
    must("linear_cross_closure_trunc_force_total", "AC3", aud)
    must("linear_cross_closure_trunc_force_total", "AC3", etc)
    must("cap_truncations && hard", "AC3", etc)
    must("force_cross_closure", "AC3", etc)
    must("ac3_trunc_prod_force", "AC3", test)

    # AC4: Soft trunc metrics only
    must("linear_cross_closure_cap_trunc_total", "AC4", etc)
    must("ac4_soft_trunc_observe", "AC4", test)

    # AC5: depth 0 disable
    if "e[0] == '0'" not in aud and 'e[0] == "0"' not in aud:
        fails.append("AC5: missing depth-0 branch in linear_cross_closure_depth_cap")
    must("depth <= 0", "AC5", tcpp)
    must("ac5_depth0_disable", "AC5", test)

    # AC6: schema + source-cite + wiring
    must("schema-2623", "AC6", q)
    must("issue-2623", "AC6", q)
    must("linear-cross-closure-trunc-force-total", "AC6", q)
    must("linear-cross-closure-depth-max", "AC6", q)
    must("linear-cross-closure-prod-depth-default", "AC6", q)
    must("#2623", "AC6", aud)
    must("#2623", "AC6", tcpp)
    must("#2623", "AC6", etc)
    must("#2623", "AC6", tci)
    must("test_linear_cross_closure_depth_trunc_2623", "AC6", cmake)
    must("check_linear_cross_closure_depth_trunc_2623", "AC6", build)
    must("cmd_linear_cross_closure_depth_trunc_coverage", "AC6", build)
    must("ac6_schema_source", "AC6", test)
    must("Issue #2623", "AC6", this)

    # AC7: #2559 inventory lists env keys
    must("LINEAR_CROSS_CLOSURE_ENV_KEYS", "AC7", inv)
    must("AURA_LINEAR_CROSS_CLOSURE_HARD", "AC7", inv)
    must("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "AC7", inv)
    must("AURA_LINEAR_CROSS_CLOSURE_HARD", "AC7", aud)
    must("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "AC7", aud)
    must("ac7_inventory_2559", "AC7", test)

    # No design docs
    for rel in (
        "docs/design/linear_cross_closure_depth_trunc_2623.md",
        "docs/linear_cross_closure_depth_trunc_2623.md",
        "design/2623.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # hard max 3
    if "kMax = 3" not in aud and "kMax=3" not in aud:
        fails.append("AC6: missing hard max kMax = 3 in depth_cap")
    must("std::min(3, raw_depth)", "AC6", tcpp)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2623 cross-closure depth + trunc fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
