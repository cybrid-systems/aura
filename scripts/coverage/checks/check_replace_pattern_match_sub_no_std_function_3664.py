#!/usr/bin/env python3
"""Issue #3664: lockless replace-pattern matcher has no std::function.

#897 closed per-call vector realloc on eval_flat dispatch. The batch
:replace-pattern recursive matcher was still a std::function Y-combinator
(heap closure + indirect call per node) on the self-modify mutate path.

Contract (one row per AC):
  AC1  eval_flat_apply_mutate_replace_pattern arm has no std::function;
       local MatchSub recurse via *this
  AC2  #3042 PureWrap still bans std::function dirty pred; while /
       DefineModule cold std::function may remain
  AC3  Soft/Off same helper; no new query key
  AC4  extend atomic-batch replace-pattern suite (wildcard + nested);
       linter AFTER #3663; no invent; no docs/design

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_atomic_batch_replace_pattern_sibling.cpp")
    build = _read("build.py")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    pw = _read("scripts/coverage/checks/check_pure_wrap_no_std_function_3042.py")
    qh = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")

    start = flat.find("eval_flat_apply_mutate_replace_pattern")
    end = flat.find("eval_flat_apply_mutate_replace_subtree")
    arm = flat[start:end] if start >= 0 and end > start else ""

    must("Issue #3664", "AC1 cite", arm)
    must("struct MatchSub", "AC1 MatchSub", arm)
    must("operator()", "AC1 recurse", arm)
    must("(*this)", "AC1 *this", arm)
    must_not("std::function", "AC1 no std::function in arm", arm)
    must("3664 AC1: no std::function in replace-pattern arm", "AC1 test", test)

    must("Issue #3042", "AC2 #3042 linter", pw)
    must("std::function", "AC2 #3042 ban", pw)
    must("has_define", "AC2 while cold", flat)
    must("node_source", "AC2 DefineModule cold", flat)
    must("3664 AC2: #3042 linter present", "AC2 test", test)

    must_not("schema-3664", "AC3 no schema", arm + mut)
    must_not("production_defaults_active", "AC3 matcher not production-gated", arm)
    must("3664 AC3: no new query key", "AC3 test", test)
    must("query:type-linear-commit-health", "AC3 commit-health key", qh + obs)

    must("(+ ... ...)", "AC4 wildcard", test)
    must("(lambda () ...)", "AC4 nested", test)
    must("3664 AC4: wildcard batch #t", "AC4 test", test)
    must("check_linear_move_elision_abort_live_3663", "AC4 prev linter", build)
    must("check_replace_pattern_match_sub_no_std_function_3664", "AC4 build.py", build)
    prev = build.find("check_linear_move_elision_abort_live_3663")
    ours = build.find("check_replace_pattern_match_sub_no_std_function_3664")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3663")
    if _read("tests/compiler/test_issue_3664.cpp") or _read("tests/issues/test_issue_3664.cpp"):
        fails.append("AC4: test_issue_3664.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3664-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print("FAIL #3664 replace_pattern_match_sub_no_std_function:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3664 replace_pattern_match_sub_no_std_function")
    return 0


if __name__ == "__main__":
    sys.exit(main())
