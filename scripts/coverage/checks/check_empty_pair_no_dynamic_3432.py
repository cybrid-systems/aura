#!/usr/bin/env python3
"""Issue #3432: covered Pair empty arm must not cache Dynamic.

#976 listed NodeTag::Pair as covered so it does not hit
note_uncovered_bidirectional_tag / #3330. Residual is the opposite:
the tag is covered, but the empty-children arm returned
dynamic_type() and cached it. Empty Pair now synthesizes fresh_var
(or Void). Soft: same fresh_var (no Dynamic compat). Quiet: Pair
with children still synthesizes the car. Do not send Pair to the
#3330 default arm.

Contract:
  AC1 Production + empty Pair → TYPE_VAR or Void, never Dynamic
  AC2 Pair with car still synthesizes the car (#976)
  AC3 Coverage table still lists Pair; #3330 default arm not taken
  AC4 no new query key; no docs/design/*; no test_issue_*.cpp;
      linter after #3330

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
    lint3044 = _read("scripts/coverage/checks/check_bidirectional_tag_coverage_3044.py")
    build = _read("build.py")

    start = impl.find("TypeId InferenceEngine::synthesize_flat")
    pair = impl.find("case Tag::Pair:", start) if start >= 0 else -1
    nxt = impl.find("case Tag::Export:", pair) if pair >= 0 else -1
    pair_body = impl[pair:nxt] if pair >= 0 and nxt > pair else ""
    dpos = impl.find("default:", start) if start >= 0 else -1

    must("kBidirectionalEmptyPairNoDynamicIssue = 3432", "AC1 stamp", hdr)
    must("Issue #3432", "AC1 cite", pair_body)
    must("cs_.fresh_var()", "AC1 fresh_var", pair_body)
    must("v.children.empty()", "AC1 empty", pair_body)
    if "result = reg_.dynamic_type()" in pair_body:
        fails.append("AC1: Pair arm still caches Dynamic")
    must("3432 AC1", "AC1 test", test)
    must("tag_of != DYNAMIC", "AC1 tag_of", test)

    must("synthesize_flat(flat, pool, v.child(0)", "AC2 car synth", pair_body)
    must("3432 AC2", "AC2 test", test)
    must("Pair with car synthesizes car", "AC2 test label", test)

    must("case T::Pair:", "AC3 coverage table", hdr)
    if pair < 0 or dpos < 0 or pair > dpos:
        fails.append("AC3: Pair case must precede #3330 default arm")
    must("note_uncovered_bidirectional_tag", "AC3 3330 default kept", impl)
    must("kBidirectionalUncoveredNoDynamicIssue = 3330", "AC3 3330 stamp", hdr)
    must("kBidirectionalUncoveredTagIssue = 3044", "AC3 3044 stamp", hdr)
    must("3432 AC3", "AC3 test", test)

    must("check_empty_pair_no_dynamic_3432", "AC4 build.py", build)
    must("3432 AC4", "AC4 test", test)
    must("3330", "AC4 3330 linter kept", lint3330)
    must("3044", "AC4 3044 linter kept", lint3044)
    prev = build.find("check_bidirectional_uncovered_no_dynamic_3330")
    ours = build.find("check_empty_pair_no_dynamic_3432")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: #3432 linter must run after #3330")
    if "schema-3432" in impl or "schema-3432" in hdr:
        fails.append("AC4: new schema-3432 query key")
    if "g_3432_" in impl or "g_3432_" in hdr:
        fails.append("AC4: new g_3432_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3432.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3432.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3432.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3432.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3432-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3432 empty_pair_no_dynamic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3432 empty_pair_no_dynamic: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
