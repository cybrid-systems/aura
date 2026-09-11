#!/usr/bin/env python3
"""Issue #3662: Production is_coercible(Dynamic, T) matches unify reject.

#3622 closed consistent_unify(Dynamic, T) under production Strict.
check_flat still called is_coercible, and Dynamic returned true before
the Strict table, so Production inserted CastOp (Note) instead of TypeError.

Contract (one row per AC):
  AC1  Production is_coercible(Dynamic, T)==false; Quote vs Int TypeError
       and take_coercions has no site
  AC2  #3622 unify Dynamic~Int / Linear~Dynamic still false
  AC3  Soft/Balanced is_coercible(Dynamic, Int)==true; Int~String kept
  AC4  Strict Float→Int still true; explicit Coercion tag not this helper
  AC5  extend test_ir #3622 suite; linter AFTER #3661; no invent;
       no query:type-linear-* rewrite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    impl = _read("src/compiler/type_checker_impl.cpp")
    tir = _read("tests/compiler/test_ir.cpp")
    build = _read("build.py")
    qh = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")

    fn = impl.find("bool InferenceEngine::is_coercible")
    win = impl[fn : fn + 2200] if fn >= 0 else ""

    must("Issue #3662", "AC1 cite", win)
    must("reg_.dynamic_type()", "AC1 Dynamic arm", win)
    must("production_defaults_active()", "AC1 production gate", win)
    must("GradualPermissiveness::Strict", "AC1 Strict", win)
    must("ac3662_1_dynamic_int_not_coercible_prod", "AC1 test", tir)
    must("ac3662_5_quote_check_typeerror_no_coercion", "AC1 check_flat test", tir)
    # Dynamic arm must be able to return false under production (not always true).
    dyn = win.find("from == reg_.dynamic_type() || to == reg_.dynamic_type()")
    dyn_win = win[dyn : dyn + 700] if dyn >= 0 else ""
    if "return true;" in dyn_win and "return false;" not in dyn_win:
        fails.append("AC1: Dynamic arm still always return true")

    must("return false; // Production: Dynamic is not a silent success (#3622)", "AC2 unify", impl)
    must("ac3622_1_dynamic_int_prod_false", "AC2 unify test", tir)
    must("ac3662_2_unify_no_regress", "AC2 test", tir)
    must("linear_of(from)", "AC2 Linear first", win)

    must("ac3662_3_soft_dynamic_coercible", "AC3 test", tir)
    must("do NOT flip this boolean", "AC3 #2992", impl)

    must("FLOAT && to_tag == TypeTag::INT", "AC4 Float→Int", win)
    must("ac3662_4_float_int_strict_and_explicit_cast", "AC4 test", tir)
    coe = impl.find("case Tag::Coercion:")
    cwin = impl[coe : coe + 500] if coe >= 0 else ""
    must_not("is_coercible", "AC4 explicit Coercion", cwin)

    must("check_packed_v2_no_occupancy_refresh_3661", "AC5 prev linter", build)
    must("check_is_coercible_dynamic_prod_3662", "AC5 build.py", build)
    prev = build.find("check_packed_v2_no_occupancy_refresh_3661")
    ours = build.find("check_is_coercible_dynamic_prod_3662")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3661")
    must("query:type-linear-commit-health", "AC5 commit-health key", qh + obs)
    must("query:type-linear-evolution-snapshot", "AC5 evolution-snapshot key", qh + obs)
    if _read("tests/compiler/test_issue_3662.cpp"):
        fails.append("AC5: test_issue_3662.cpp present")
    if _read("docs/design/3662-is-coercible-dynamic.md"):
        fails.append("AC5: docs/design/ exists")
    must_not("schema-3662", "AC5 no new query key", impl)
    must("if (is_coercible(inferred, expected))", "AC5 check_flat still uses helper", impl)

    if fails:
        print("FAIL #3662 is_coercible_dynamic_prod:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3662 is_coercible_dynamic_prod")
    return 0


if __name__ == "__main__":
    sys.exit(main())
