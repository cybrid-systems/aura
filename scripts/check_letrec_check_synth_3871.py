#!/usr/bin/env python3
"""Issue #3871 — LetRec unannotated check_flat must synthesize the value.

Residual: check_flat Let/LetRec had `else if (!is_rec)` — an unannotated
LetRec value was never walked in check mode, so errors inside the recursive
body went unreported (synthesize_flat_let always walked it; infer/check
primary path covered the Let case only).

  AC1  else-arm-walks:     the check_flat Let/LetRec branch walks the value
      in the unannotated arm (synthesize_flat call), has no `else if
      (!is_rec)` guard, cites Issue #3871, and unifies the rec forward var.
  AC2  synth-path-ssot:    synthesize_flat_let still owns the LetRec
      synthesis semantics (forward bind → synthesize → unify unchanged).
  AC3  test-wired:         test_bidirectional_annotation.cpp (built host)
      carries the #3871 behavioral ACs.
  AC4  no-invent:          no docs/design/3871-*, no test_issue_3871.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the branch detector over
an inline stale fixture (with `else if (!is_rec)`) and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TCI = ROOT / "src" / "compiler" / "type_checker_impl.cpp"
TEST = ROOT / "tests" / "compiler" / "test_bidirectional_annotation.cpp"


def let_branch(text):
    start = text.find("InferenceEngine::check_flat(")
    if start < 0:
        return None
    after = text[start:]
    let_pos = after.find("NodeTag::Let || v.tag == NodeTag::LetRec")
    if let_pos < 0:
        return None
    begin_pos = after.find("NodeTag::Begin", let_pos)
    if begin_pos < 0:
        return after[let_pos:]
    return after[let_pos:begin_pos]


def ac1_else_arm_walks():
    text = TCI.read_text()
    branch = let_branch(text)
    ok = (
        branch is not None
        and "Issue #3871" in branch
        and "} else if (!is_rec) {" not in branch
        and "synthesize_flat(flat, pool, val_id" in branch
        and "consistent_unify(rec_fwd, val_type)" in branch
    )
    print("AC(else-arm-walks): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_synth_path_unchanged():
    text = TCI.read_text()
    start = text.find("TypeId InferenceEngine::synthesize_flat_let(")
    ok = start >= 0
    if ok:
        nxt = text.find("TypeId InferenceEngine::", start + 10)
        body = text[start : nxt if nxt > 0 else len(text)]
        ok = "consistent_unify(fwd_var, val_type)" in body and "synthesize_flat(flat, pool, v.child(0)" in body
    print("AC(synth-path-ssot): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_test_wired():
    text = TEST.read_text()
    ok = "#3871 AC1" in text and "3871 AC4" in text
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_no_invent():
    docs = list(ROOT.glob("docs/design/3871-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3871.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test():
    fixture = (
        "InferenceEngine::check_flat() {\n"
        "    if (v.tag == NodeTag::Let || v.tag == NodeTag::LetRec) {\n"
        "        if (val_expected.valid()) {\n"
        "            check_flat(flat, pool, val_id, val_expected);\n"
        "        } else if (!is_rec) {\n"
        "            TypeId val_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));\n"
        "            env_.bind(var_name, val_type);\n"
        "        }\n"
        "    } else if (v.tag == NodeTag::Begin) {\n"
    )
    branch = let_branch(fixture)
    flagged = branch is not None and "else if (!is_rec)" in branch
    print("self-test: " + ("PASS — stale guard detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_else_arm_walks(),
        ac2_synth_path_unchanged(),
        ac3_test_wired(),
        ac4_no_invent(),
    ]
    print(f"check_letrec_check_synth_3871: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
