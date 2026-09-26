#!/usr/bin/env python3
"""Issue #4107: production if-join and arithmetic peel publish a solved
type without the ground gate (P1).

Hole: ConstraintSystem::consistent_unify under the production face
fail-closes ground T ~ Dynamic (#3622), but two synthesis paths never call
it. They cache a solved type, solve stays SOLVED, and the type export
stays authoritative — an unannotated Agent edit ships a green query:type
for a ground mismatch:

  - InferenceEngine::lub returned dynamic_type() for any two ids that are
    neither identical, neither Dynamic, and not the Int/Float promotion
    pair. synthesize_flat_if joins both branches through it, so
    `(if p 1 "a")` synthesized an authoritative Dynamic;
  - InferenceEngine::synthesize_flat_call_arith returned int_type() when
    both arguments were concrete and neither pair was Int/Float (the
    `(+ "42" 1)` runtime-will-coerce comment), and returned the operand
    type for a single concrete argument (`(+ "a")` was an authoritative
    String).

Fix shape (no new constraint kind, no metrics field, consistent_unify and
the soft gradual join untouched):
  - lub: Int/Float promotion, both Dynamic arms, and the soft dynamic
    fallback unchanged. Under production_hard_face_active() ||
    production_defaults_active() a type variable still returns so a later
    unify can bind it; any other pair of distinct grounds reports a hard
    TypeError and returns void_type() — never a successful Dynamic.
  - synthesize_flat_call_arith: the Int/Float and var-unify arms are
    unchanged. Under the production face two ground non-numeric operands,
    or one ground non-numeric operand, take the same TypeError and return
    void_type(); soft keeps the historical Int / operand pass-through and
    a Dynamic operand keeps the gradual escape in both faces.
  - infer_flat captures the diagnostic count before synthesis and demotes
    the existing last_type_export_authoritative_ store when a TypeError
    was reported during synthesis under the production face.

Contract (one row per AC):
  AC1  lub under the production face: distinct grounds → TypeError +
       void_type(); Int/Float promotion, Dynamic arms, var fallback, and
       the soft dynamic fallback unchanged
  AC2  arith peel, two ground non-numeric operands: production TypeError +
       void_type(); soft runtime-coerce Int retained; Dynamic operand
       escape retained
  AC3  arith peel, single ground non-numeric operand: production
       TypeError + void_type(); soft operand pass-through retained
  AC4  infer_flat captures the pre-synthesis diagnostic count and demotes
       last_type_export_authoritative_ when a synthesis TypeError was
       reported under the production face; soft face unchanged
  AC5  tests/compiler/test_ir.cpp carries the #4107 ACs (production
       if-join / arith pair / arith single non-authoritative, soft twins,
       numeric arms Int/Float, ground~Dynamic consistent_unify, annotated
       check_flat_if mismatch); no tests/**/test_issue_4107.cpp tree
  AC6  build.py + root allowlist wiring; no docs/design/4107-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def window(src: str, begin: str, end: str, label: str, width: int = 5200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    impl = _read("src/compiler/type_checker_impl.cpp")
    test = _read("tests/compiler/test_ir.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: lub production ground gate ──────────────────────────────────
    lub = window(impl, "TypeId InferenceEngine::lub(TypeId a, TypeId b) {", "// FlatAST Inference (bypasses", "AC1 lub")
    must("Issue #4107", "AC1 cites the issue", lub)
    must("production_hard_face_active()", "AC1 hard-face gate", lub)
    must("production_defaults_active()", "AC1 production-defaults gate", lub)
    must("reg_.is_var(a) || reg_.is_var(b)", "AC1 type variable still returns", lub)
    must("return reg_.dynamic_type(); // later unify binds the variable", "AC1 var fallback so later unify binds", lub)
    must("ErrorKind::TypeError", "AC1 hard TypeError", lub)
    must('"incompatible ground types: if branches join "', "AC1 diagnostic names the join", lub)
    must("return reg_.void_type();", "AC1 void result, not a Dynamic", lub)
    must("if (a == reg_.dynamic_type() || b == reg_.dynamic_type())", "AC1 Dynamic arms unchanged", lub)
    must('a == reg_.int_type() && b == reg_.lookup_type("Float")', "AC1 Int/Float promotion unchanged", lub)
    must("return reg_.dynamic_type(); // safe fallback", "AC1 soft fallback retained", lub)
    absent("Constraint::GROUND", "AC1 no new constraint kind", lub)

    # ── AC2: arith peel, two ground non-numeric operands ─────────────────
    arith_pair = window(
        impl, "// Both concrete but not INT/FLOAT", "// At least one is a type variable", "AC2 arith pair"
    )
    must("Issue #4107", "AC2 cites the issue", arith_pair)
    must("production_hard_face_active()", "AC2 hard-face gate", arith_pair)
    must("production_defaults_active()", "AC2 production-defaults gate", arith_pair)
    must("tag0 != TypeTag::DYNAMIC && tag1 != TypeTag::DYNAMIC", "AC2 Dynamic operand escape retained", arith_pair)
    must(
        'ErrorKind::TypeError,\n                                    "incompatible ground types: arithmetic operands "',
        "AC2 hard TypeError",
        arith_pair,
    )
    must("return reg_.void_type();", "AC2 void result", arith_pair)
    must(
        "// Return Int for arithmetic (runtime handles coercion)\n        return reg_.int_type();",
        "AC2 soft runtime-coerce Int retained",
        arith_pair,
    )

    # ── AC3: arith peel, single ground non-numeric operand ───────────────
    arith_one = window(impl, "// Variadic arith with 0 or 1 args", "// Synthesize arg types", "AC3 arith single")
    must("Issue #4107", "AC3 cites the issue", arith_one)
    must("production_hard_face_active()", "AC3 hard-face gate", arith_one)
    must("production_defaults_active()", "AC3 production-defaults gate", arith_one)
    must(
        "tag0 != TypeTag::INT && tag0 != TypeTag::FLOAT && tag0 != TypeTag::DYNAMIC",
        "AC3 numeric + Dynamic escape retained",
        arith_one,
    )
    must('"incompatible ground types: arithmetic operand "', "AC3 hard TypeError", arith_one)
    must("return reg_.void_type();", "AC3 void result", arith_one)
    must("return t0;", "AC3 soft operand pass-through retained", arith_one)

    # ── AC4: infer_flat authority demotion on synthesis TypeError ────────
    demote = window(
        impl,
        "// Issue #4107: capture the diagnostic count before synthesis",
        "// Individual sub-nodes' caches",
        "AC4 demotion",
    )
    must(
        "const std::size_t diag_count_before_synthesis = diag_.diagnostics().size();",
        "AC4 pre-synthesis diagnostic count capture",
        demote,
    )
    must("Issue #4107", "AC4 cites the issue", demote)
    must("production_defaults_active() ||", "AC4 production gate", demote)
    must("aura::compiler::typed_audit::production_hard_face_active()", "AC4 hard-face gate", demote)
    must("diags_after[i].kind == ErrorKind::TypeError", "AC4 synthesis TypeError detection", demote)
    must("last_type_export_authoritative_ = !synth_type_error;", "AC4 demoted authoritative store", demote)

    # ── AC5: test ACs in tests/compiler/test_ir.cpp, no new tree ─────────
    for n in (
        "Issue #4107",
        "ac4107_1_if_join_prod_not_authoritative",
        "ac4107_1_if_join_soft_dynamic",
        "ac4107_2_arith_pair_prod_not_int",
        "ac4107_2_arith_pair_soft_int",
        "ac4107_3_arith_single_prod_not_string",
        "ac4107_3_arith_single_soft_string",
        "ac4107_4_numeric_arms_int_float",
        "ac4107_5_ground_dynamic_prod_false",
        "ac4107_5_ground_dynamic_soft_true",
        "ac4107_5_annotated_if_branch_mismatch",
        "tc.type_export_is_authoritative()",
    ):
        must(n, f"AC5 test row {n}", test)
    absent("test_issue_4107.cpp", "AC5 no new test tree", test)

    # ── AC6: gate wiring ─────────────────────────────────────────────────
    must('tg4107_script = ROOT / "scripts" / "check_type_ground_gate_4107.py"', "AC6 build.py script wiring", build)
    must("Issue #4107 type ground gate linter failed", "AC6 build.py fail message", build)
    must("check_type_ground_gate_4107.py", "AC6 root allowlist row", allow)
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir() and any(design_dir.glob("4107-*")):
        fails.append("AC6: docs/design/4107-* must not exist (per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("OK: #4107 type ground gate — all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
