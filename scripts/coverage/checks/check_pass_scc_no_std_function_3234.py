#!/usr/bin/env python3
"""Issue #3234: compute_sccs Tarjan uses local recursive struct, not type erasure.

#3042 closed std::function dirty predicates on PureWrap. Residual: Tarjan
strongconnect in pass_impls.ixx still heap-allocated a self-referential
closure. Production pass surface must contain zero std::function.

Contract:
  AC1 grep std::function in pass_impls.ixx returns 0; StrongConnect present
  AC2 existing inliner / SCC-dependent suite still cites compute_sccs
  AC3 same Tarjan body (lowlink / on_stack / reverse-topo scc_id)
  AC4 source-cite; linter wired; no test_issue_*.cpp; no docs/design/

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

    impls = _read("src/compiler/pass_impls.ixx")
    test = _read("tests/compiler/test_hot_pass_hard_dod.cpp")
    build = _read("build.py")
    pw3042 = _read("scripts/coverage/checks/check_pure_wrap_no_std_function_3042.py")

    if "std::function" in impls:
        fails.append("AC1: std::function still in src/compiler/pass_impls.ixx")
    if "#include <functional>" in impls:
        fails.append("AC1: <functional> still included in pass_impls.ixx")
    must("struct StrongConnect", "AC1 struct", impls)
    must("kPassSccNoStdFunctionIssue = 3234", "AC1 stamp", impls)
    must("(*this)(w)", "AC1 recurse", impls)

    must("compute_sccs", "AC2 inliner", impls)
    must("scc_id_of_fid_", "AC2 inliner scc", impls)
    must("3234 AC1: no std::function", "AC1 test", test)
    must("3234 AC2: InlinePass / SCC-dependent run still green", "AC2 test", test)

    must("lowlink", "AC3 lowlink", impls)
    must("on_stack", "AC3 on_stack", impls)
    must("numbered in reverse topological order", "AC3 reverse-topo", impls)
    must("3234 AC3: reverse-topo", "AC3 test", test)

    must("Issue #3234", "AC4 cite", impls)
    must("check_pass_scc_no_std_function_3234", "AC4 build.py", build)
    must("3234 closed the Tarjan leftover", "AC4 3042 linter", pw3042)
    must("3234 AC4: no invent", "AC4 test", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3234.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3234.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3234.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3234.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3234-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3234 pass_scc_no_std_function:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3234 pass_scc_no_std_function: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
