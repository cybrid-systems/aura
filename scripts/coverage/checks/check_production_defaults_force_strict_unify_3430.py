#!/usr/bin/env python3
"""Issue #3430: production_defaults forces Strict unify without set_strict.

#3202 hard-rejects Int~String only when unify_gradual_mode_ is Strict AND
production_defaults_active(). Default gradual is Balanced; set_strict
ran only on the Hard full recheck after infer_flat_partial. Production
first-pass infer was still Balanced. effective_gradual_permissiveness()
now returns Strict under production_defaults (both InferenceEngine and
TypeChecker). Soft / Off / env Balanced unchanged.

Contract:
  AC1 Production + no set_strict + Int~String → unify false; error
      counter; no CastOp
  AC2 infer_flat_partial first pass uses Strict (not only later Hard)
  AC3 Soft / Off / Balanced env: #2992 Warning path + Int↔Float
  AC4 Dynamic ~ T and Linear+Dynamic reject (#117) unchanged
  AC5 no docs/design/*; no test_issue_*.cpp; linter after #3202;
      no new query key

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

    tix = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ta = _read("tests/compiler/test_bidirectional_annotation.cpp")
    tir = _read("tests/compiler/test_ir.cpp")
    build = _read("build.py")
    lint3202 = _read("scripts/coverage/checks/check_production_strict_ground_unify_3202.py")

    must("kProductionDefaultsForceStrictUnifyIssue = 3430", "AC1 stamp", tix)
    must("Issue #3430", "AC1 cite ixx", tix)
    must("ac3430_1_effective_strict_without_set_strict", "AC1 test", ta)
    must("ac3430_1_no_castop", "AC1 no CastOp", ta)
    must("ac3430_1_error_counter", "AC1 counter", ta)
    must("ac3430_1_effective_strict_without_set_strict", "AC1 live test_ir", tir)

    ie = tix.find("[[nodiscard]] GradualPermissiveness effective_gradual_permissiveness()")
    if ie < 0:
        fails.append("AC1: InferenceEngine effective_gradual_permissiveness missing")
    else:
        ie_win = tix[ie : ie + 700]
        if "production_defaults_active()" not in ie_win:
            fails.append("AC1: InferenceEngine effective() must consult production_defaults")
        if "return GradualPermissiveness::Strict;" not in ie_win:
            fails.append("AC1: InferenceEngine effective() must return Strict under production")

    tc = tix.rfind("[[nodiscard]] GradualPermissiveness effective_gradual_permissiveness()")
    if tc < 0 or tc == ie:
        fails.append("AC1: TypeChecker effective_gradual_permissiveness copy missing")
    else:
        tc_win = tix[tc : tc + 700]
        if "production_defaults_active()" not in tc_win:
            fails.append("AC1: TypeChecker effective() must consult production_defaults")

    must("ac3430_2_partial_entry", "AC2 test", ta)
    must("ac3430_2_live_effective_strict", "AC2 live", ta)
    must("ac3430_2_live_effective_strict", "AC2 live test_ir", tir)
    must("engine.set_gradual_permissiveness(gradual_permissiveness_)", "AC2 partial plumb", impl)
    must("TypeChecker::infer_flat_partial", "AC2 partial", impl)

    must("ac3430_3_soft_balanced_unify_true", "AC3 Soft Balanced", ta)
    must("ac3430_3_numeric_int_float", "AC3 Int↔Float", ta)
    must("ac3430_3_soft_balanced_unify_true", "AC3 live test_ir", tir)
    must("ac2992_4_permissive_silent", "AC3 2992 retained", ta)

    must("ac3430_4_dynamic_permissive", "AC4 Dynamic", ta)
    must("ac3430_4_linear_dynamic_reject", "AC4 Linear+Dynamic", ta)
    must("Issue #117", "AC4 #117 cite", impl)
    must("ac3430_4_dynamic_and_linear", "AC4 live test_ir", tir)

    must("check_production_defaults_force_strict_unify_3430", "AC5 build.py", build)
    must("ac3430_5_no_docs", "AC5 test", ta)
    must("3202", "AC5 3202 linter kept", lint3202)
    prev = build.find("check_production_strict_ground_unify_3202")
    ours = build.find("check_production_defaults_force_strict_unify_3430")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3430 linter must run after #3202")
    if "schema-3430" in tix or "schema-3430" in impl:
        fails.append("AC5: new schema-3430 query key")
    if "g_3430_" in tix or "g_3430_" in impl:
        fails.append("AC5: new g_3430_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3430.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3430.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3430.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3430.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3430-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3430 production_defaults_force_strict_unify:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3430 production_defaults_force_strict_unify: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
