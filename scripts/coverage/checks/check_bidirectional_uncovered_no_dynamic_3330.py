#!/usr/bin/env python3
"""Issue #3330: Production synthesize_flat default must not return Dynamic.

#3044 notes uncovered tags (Hard → TypeError) but still assigned
result = dynamic_type(). Under Production, consistent_unify(Dynamic, T)
succeeds so Agent-generated tags type-check green. #3330 fail-closes:
hard → void_type(), no cache, no clear_dirty. Soft keeps Dynamic.

Contract:
  AC1 Production/strict uncovered tag → no Dynamic TypeId written
  AC2 Soft Warning + Dynamic
  AC3 covered tags zero extra (gate only on default)
  AC4 extend 3044 linter + test_bidirectional_match_check; no invent / docs

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
    cov = _read("scripts/coverage/checks/check_bidirectional_tag_coverage_3044.py")
    build = _read("build.py")

    start = impl.find("TypeId InferenceEngine::synthesize_flat")
    default = impl[start : start + 9000] if start >= 0 else ""
    dpos = default.rfind("default:")
    darm = default[dpos : dpos + 1200] if dpos >= 0 else ""

    must("kBidirectionalUncoveredNoDynamicIssue = 3330", "AC1 stamp", hdr)
    must("const bool hard = note_uncovered_bidirectional_tag", "AC1 hard", darm)
    must("return reg_.void_type()", "AC1 void", darm)
    must("never Dynamic", "AC1 never Dynamic", darm)
    must("3330 AC1: no Dynamic TypeId written", "AC1 test", test)
    if "result = reg_.dynamic_type()" in darm and "if (hard)" in darm:
        # Soft arm may still assign Dynamic; hard must return before that.
        hard_ret = darm.find("if (hard)")
        dyn = darm.find("result = reg_.dynamic_type()")
        if dyn >= 0 and hard_ret >= 0 and dyn < hard_ret:
            fails.append("AC1: Dynamic assigned before hard return")

    must("Soft only", "AC2 Soft Dynamic", darm)
    must("3330 AC2: Soft keeps Dynamic", "AC2 test", test)

    must("zero extra load", "AC3", darm)
    must("3044 AC3: covered path zero extra stores", "AC3 3044", test)

    must("Issue #3330", "AC4 3044 linter", cov)
    must("check_bidirectional_uncovered_no_dynamic_3330", "AC4 build.py", build)
    must("kBidirectionalUncoveredTagIssue = 3044", "AC4 3044 lineage", hdr)
    prev = build.find("check_bidirectional_tag_coverage_3044")
    ours = build.find("check_bidirectional_uncovered_no_dynamic_3330")
    if ours < 0:
        fails.append("AC4: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3044")

    if "g_3330_" in impl or "g_3330_" in hdr:
        fails.append("AC4: new g_3330_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3330.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3330.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3330-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3330 bidirectional_uncovered_no_dynamic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3330 bidirectional_uncovered_no_dynamic: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
