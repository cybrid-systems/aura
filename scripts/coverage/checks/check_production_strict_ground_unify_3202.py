#!/usr/bin/env python3
"""Issue #3202: Production + Strict ground unify hard-rejects Int~String.

Residual of #2992: consistent_unify boolean stayed true for any two
concrete grounds. Production + Strict now returns false (except Int↔Float
and Dynamic~T) so Agents get early TypeError / no CastOp. Soft / balanced
/ permissive keep the diagnostic-only path. Quiet Balanced skips the
production_defaults_active load.

Contract:
  AC1 Production + Strict → unify false; TypeError; no CastOp
  AC2 Soft / balanced / permissive unify stays true (#2992)
  AC3 Dynamic ~ T and Int ↔ Float stay permissive
  AC4 default balanced path unchanged (no extra production load)
  AC5 this linter + existing bidirectional suite; no docs/design / invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    tix = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ta = _read("tests/compiler/test_bidirectional_annotation.cpp")
    tir = _read("tests/compiler/test_ir.cpp")
    build = _read("build.py")

    must("kProductionStrictGroundUnifyIssue = 3202", "AC1 stamp", tix)
    must("set_unify_gradual_mode", "AC1 CS setter", tix)
    must("Issue #3202", "AC1 cite ixx", tix)
    must("Issue #3202", "AC1 cite impl", impl)

    ground = re.search(
        r"if \(!reg_\.is_var\(t1\) && !reg_\.is_var\(t2\) && !f1 && !f2\)\s*\{(?P<body>.*?)\n    \}",
        impl,
        re.DOTALL,
    )
    if not ground:
        fails.append("AC1: could not extract consistent_unify ground branch")
    else:
        body = ground.group("body")
        if "unify_gradual_mode_ == GradualPermissiveness::Strict" not in body:
            fails.append("AC4: Strict mode check must precede production_defaults load")
        strict_idx = body.find("unify_gradual_mode_ == GradualPermissiveness::Strict")
        prod_idx = body.find("production_defaults_active")
        if strict_idx < 0 or prod_idx < 0 or strict_idx > prod_idx:
            fails.append("AC4: quiet Balanced must skip production_defaults_active")
        if "is_intentional_numeric_coercion" not in body:
            fails.append("AC3: Int↔Float allow-list missing")
        if "return false" not in body:
            fails.append("AC1: hard-reject return false missing")
        if "do NOT flip this boolean" not in body:
            fails.append("AC2: #2992 Soft return true (do NOT flip this boolean) missing")
        if "gradual_ground_incompatible_error_total" not in body:
            fails.append("AC1: must bump existing ground-incompatible error counter")

    must("effective_gradual_permissiveness() == GradualPermissiveness::Strict", "AC1 is_coercible", impl)
    must("ac3202_1_prod_strict_unify_false", "AC1 test", ta)
    must("ac3202_1_no_castop", "AC1 no CastOp", ta)
    must("ac3202_2_soft_strict_unify_true", "AC2 test", ta)
    must("ac3202_2_prod_balanced_unify_true", "AC2 balanced", ta)
    must("ac3202_3_dynamic_permissive", "AC3 Dynamic", ta)
    must("ac3202_3_numeric_int_float", "AC3 numeric", ta)
    must("ac3202_4_source_and_linter", "AC5 test", ta)
    must("ac3202_1_prod_strict_unify_false", "AC1 live test_ir", tir)
    must("ac3202_2_soft_strict_unify_true", "AC2 live test_ir", tir)
    must("ac3202_3_dynamic_permissive", "AC3 live test_ir", tir)
    must("check_production_strict_ground_unify_3202", "AC5 build.py", build)

    if "query:production-strict-ground-unify" in impl or "query:production-strict-ground-unify" in tix:
        fails.append("AC5: new public query key")
    if "g_3202_" in impl or "g_3202_" in tix:
        fails.append("AC5: invented g_3202_* counter")

    if (ROOT / "tests" / "compiler" / "test_issue_3202.cpp").is_file():
        fails.append("AC5: test_issue_3202.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3202-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3202 production Strict ground unify hard-reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
