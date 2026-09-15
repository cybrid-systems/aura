#!/usr/bin/env python3
"""Issue #3830: covered TypeAnnotation empty arm must not cache Dynamic.

Sibling incomplete shapes (empty Pair #3432 / Linear+Call #3518) already
use fresh_var(). synthesize_flat_annotation returned dynamic_type() when
children.empty(). Soft/Production now synthesize fresh_var; check against
expected unifies like a hole. Non-empty / :? / _ holes unchanged.
Uncovered-tag Production fail-closed (#3330) unchanged.

Contract:
  AC1 Soft/Production empty TypeAnnotation synth ≠ Dynamic; check vs Int
      like fresh_var hole
  AC2 Non-empty annotation / :? / _ holes unchanged
  AC3 Uncovered-tag Production fail-closed unchanged; suite / stamp /
      build wiring; no invent test_issue_*.cpp; no docs/design/

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

    impl = _read("src/compiler/type_checker_impl.cpp")
    hdr = _read("src/compiler/type_checker.ixx")
    test = _read("tests/compiler/test_bidirectional_match_check.cpp")
    lint3330 = _read("scripts/coverage/checks/check_bidirectional_uncovered_no_dynamic_3330.py")
    build = _read("build.py")

    must("kBidirectionalEmptyTypeAnnotationNoDynamicIssue = 3830", "AC1 stamp", hdr)
    must("Issue #3830", "AC1 cite", impl)

    ann_fn = impl.find("TypeId InferenceEngine::synthesize_flat_annotation")
    if ann_fn < 0:
        fails.append("AC1: synthesize_flat_annotation missing")
        ann_body = ""
    else:
        # Peel until next top-level InferenceEngine:: member.
        nxt = impl.find("\nTypeId InferenceEngine::", ann_fn + 10)
        if nxt < 0:
            nxt = impl.find("\nbool InferenceEngine::", ann_fn + 10)
        ann_body = impl[ann_fn : nxt if nxt > ann_fn else ann_fn + 1200]
    must("cs_.fresh_var()", "AC1 fresh_var", ann_body)
    must("v.children.empty()", "AC1 empty", ann_body)
    if "return reg_.dynamic_type()" in ann_body:
        fails.append("AC1: synthesize_flat_annotation still returns Dynamic on empty")
    must("3830 AC1", "AC1 test", test)
    must("tag_of != DYNAMIC", "AC1 tag_of", test)
    must("fresh_var hole unifies with Int", "AC1 hole vs Int", test)

    must("is_type_hole", "AC2 hole helper", ann_body)
    must("3830 AC2", "AC2 test", test)
    must(":? hole still synthesizes inner", "AC2 :? hole", test)
    must("_ hole still synthesizes inner", "AC2 _ hole", test)

    must("kBidirectionalUncoveredNoDynamicIssue = 3330", "AC3 3330 stamp", hdr)
    must("note_uncovered_bidirectional_tag", "AC3 3330 default kept", impl)
    must("3830 AC3", "AC3 test", test)
    must("check_empty_type_annotation_no_dynamic_3830", "AC3 build.py", build)
    must("3330", "AC3 3330 linter kept", lint3330)
    if "schema-3830" in impl or "schema-3830" in hdr:
        fails.append("AC3: new schema-3830 query key")
    if "g_3830_" in impl or "g_3830_" in hdr:
        fails.append("AC3: new g_3830_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3830.cpp").is_file():
        fails.append("AC3: forbidden tests/compiler/test_issue_3830.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3830.cpp").is_file():
        fails.append("AC3: forbidden tests/issues/test_issue_3830.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3830-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden #1655)")

    # check_flat empty arm must unify like fresh_var hole
    check_pos = impl.find("InferenceEngine::check_flat(")
    check_after = impl[check_pos:] if check_pos >= 0 else ""
    ta_pos = check_after.find("NodeTag::TypeAnnotation")
    ta_branch = check_after[ta_pos : ta_pos + 900] if ta_pos >= 0 else ""
    must("Issue #3830", "AC1 check cite", ta_branch)
    must("consistent_unify(inferred, expected)", "AC1 check hole", ta_branch)

    if fails:
        print("FAIL #3830 empty_type_annotation_no_dynamic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3830 empty_type_annotation_no_dynamic: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
