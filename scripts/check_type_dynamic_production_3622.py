#!/usr/bin/env python3
# scripts/check_type_dynamic_production_3622.py -- Issue #3622 source-cite gate.
#
# AC1: Production face — consistent_unify's Dynamic arm fails closed:
#      under Strict + production_defaults_active the gradual accept arm
#      bumps dynamic_degrade_with_blame_total (#2064 blame) and returns
#      false (Dynamic is not a silent success). Soft / Balanced keep the
#      gradual core; Dynamic ~ Linear stays false (#117).
# AC2: Production + Strict ground table rejects ANY two concrete non-var
#      grounds with unequal tags (not only primitive pairs) — List~Int,
#      distinct ADT names, PAIR~VECTOR — except the intentional
#      Int↔Float coercion. The old primitive-only row is gone.
# AC3: Soft / Balanced path stays true (#2992 "do NOT flip this boolean")
#      and maybe_report_ground_inconsistency stays untouched.
# AC4: No new counter, no new query key (reuse dynamic_degrade_with_blame
#      + gradual_ground_incompatible_error_total faces).
# AC5: ACs in the existing Strict ground suite (tests/compiler/test_ir.cpp
#      ac3622_*; ac3202_3 flipped in test_ir + test_bidirectional_
#      annotation); no test_issue_3622.cpp, no docs/design/*3622*.
# AC6: Source-cites ConstraintSystem::consistent_unify.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

IMPL = "src/compiler/type_checker_impl.cpp"
TIR = "tests/compiler/test_ir.cpp"
TBI = "tests/compiler/test_bidirectional_annotation.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_type_dynamic_production_3622"
DYN_REJECT = "return false; // Production: Dynamic is not a silent success (#3622)"
GATE = "unify_gradual_mode_ == GradualPermissiveness::Strict &&"
OLD_PRIM_ROW = "is_prim(a) && is_prim(b) && a != b"

AC_FN = (
    "ac3622_1_dynamic_int_prod_false",
    "ac3622_2_list_int_prod_false",
    "ac3622_3_dynamic_soft_true",
    "ac3622_4_int_float_prod_true",
    "ac3622_5_linear_dynamic_false",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(impl: str, tir: str, tbi: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — Dynamic arm fails closed under the production face.
    dyn = impl.find("Issue #3622: Production face — Dynamic ~ T is not a silent")
    if dyn == -1:
        fails.append("AC1: Dynamic production reject missing")
        win = ""
    else:
        win = impl[dyn : dyn + 1400]
    must(GATE, "AC1 strict gate precedes", win)
    must("typed_audit::production_defaults_active()", "AC1 production face gate", win)
    must(DYN_REJECT, "AC1 Dynamic not a silent success", win)
    must("dynamic_degrade_with_blame_total", "AC1 #2064 blame note on reject", win)
    must("Dynamic ~ Linear already failed closed above (#117)", "AC1 Linear kept", win)

    # AC2 — ground table rejects any unequal concrete tags.
    must("Issue #3622: Production + Strict hard-rejects ANY two", "AC2 ground table cite", impl)
    must("if (a != b && !is_intentional_numeric_coercion)", "AC2 unequal-tag reject", impl)
    must_not(OLD_PRIM_ROW, "AC2 primitive-only row removed", impl)
    must("is_intentional_numeric_coercion =", "AC2 Int↔Float allow-list kept", impl)
    must("gradual_ground_incompatible_error_total", "AC2 existing error counter reused", impl)

    # AC3 — Soft / Balanced gradual core + #2992 diagnostic untouched.
    must("do NOT flip this boolean", "AC3 #2992 soft return true cite", impl)
    must("Issue #2992 / #3202: Agent-facing diagnostic", "AC3 diagnostic helper untouched", impl)

    # AC4 — no new counter / query key.
    must_not("g_3622_", "AC4 no new counter", impl)
    must_not("schema-3622", "AC4 no new query key", impl)

    # AC5 — ACs in the existing Strict ground suite; flipped fixtures cited.
    for fn in AC_FN:
        must(fn, "AC5 test AC defined", tir)
    must("Issue #3622: Production consistent_unify rejects Dynamic~T", "AC5 block cite", tir)
    must("!cs.consistent_unify(treg.dynamic_type(), treg.string_type())", "AC5 tir flip", tir)
    must("Issue #3622 residual", "AC5 tbi flip cite", tbi)
    must("!cs.consistent_unify(reg.dynamic_type(), reg.string_type())", "AC5 tbi flip", tbi)
    must("ac3202_3_dynamic_permissive", "AC5 #3202 linter anchor kept (tir)", tir)
    must("ac3202_3_dynamic_permissive", "AC5 #3202 linter anchor kept (tbi)", tbi)
    if (ROOT / "tests" / "compiler" / "test_issue_3622.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3622.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "3622" in p.name:
                fails.append(f"AC4: forbidden docs/design/{p.name} (#1655)")
                break

    # Wiring.
    must(LINTER, "wiring build.py", build)
    must(LINTER + ".py", "wiring allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_impl = (
            "Issue #3622: Production face — Dynamic ~ T is not a silent\n"
            "unify_gradual_mode_ == GradualPermissiveness::Strict &&\n"
            "typed_audit::production_defaults_active()\n"
            "dynamic_degrade_with_blame_total\n" + DYN_REJECT + "\n"
            "Dynamic ~ Linear already failed closed above (#117)\n"
            "Issue #3622: Production + Strict hard-rejects ANY two\n"
            "if (a != b && !is_intentional_numeric_coercion)\n"
            "is_intentional_numeric_coercion =\n"
            "gradual_ground_incompatible_error_total\n"
            "do NOT flip this boolean\n"
            "Issue #2992 / #3202: Agent-facing diagnostic\n"
        )
        sample_tir = (
            "Issue #3622: Production consistent_unify rejects Dynamic~T\n"
            + "".join(fn + "\n" for fn in AC_FN)
            + "!cs.consistent_unify(treg.dynamic_type(), treg.string_type())\n"
            "ac3202_3_dynamic_permissive\n"
        )
        sample_tbi = (
            "Issue #3622 residual\n"
            "!cs.consistent_unify(reg.dynamic_type(), reg.string_type())\n"
            "ac3202_3_dynamic_permissive\n"
        )
        ok_fails = _rows(sample_impl, sample_tir, sample_tbi, LINTER, LINTER + ".py")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "", "")
        if len(neg_fails) < 12:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(IMPL), _read(TIR), _read(TBI), _read(BUILD), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("type dynamic production (#3622) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
