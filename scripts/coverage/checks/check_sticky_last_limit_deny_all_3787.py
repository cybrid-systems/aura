#!/usr/bin/env python3
"""Issue #3787: sticky global last_limit 8/9/10 must not false-deny via deny_all.

AC1  deny_all uses get_fiber_hygiene_metrics(...).last_limit_reason for 8/9/10
AC2  own-walk deny still excludes 8/9/10 (#3685)
AC3  clear-on-success for 8/9/10 retained (#3756)
AC4  tests + build.py + grandfather; no test_issue_3787.cpp
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    path = ROOT / rel
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    me = _read("src/compiler/macro_expansion.cpp")
    test = _read("tests/compiler/test_concurrent_clone_hygiene_depth.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")

    must("Issue #3787", "AC1 cite", me)
    must("get_fiber_hygiene_metrics(fid).last_limit_reason", "AC1 per-fiber", me)
    # own-walk still excludes 8/9/10
    must("Issue #3685", "AC2 cite", me)
    must("kHygieneLimitReasonNameMapShared", "AC2 name-map code", me)
    # #3756 clear retained
    must("Issue #3756", "AC3 clear cite", me)
    must("kHygieneLimitReasonConcurrentTopLevel", "AC3 clear codes", me)

    must("3787 AC1", "AC4 test", test)
    must("check_sticky_last_limit_deny_all_3787", "AC4 build.py", build)
    must("check_sticky_last_limit_deny_all_3787.py", "AC4 grandfather", gf)

    if _read("tests/compiler/test_issue_3787.cpp"):
        fails.append("AC4: test_issue_3787.cpp present")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for f in sorted(design.glob("3787-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print(f"FAIL #3787 sticky_last_limit_deny_all ({len(fails)} rows):")
        for row in fails:
            print(f"  - {row}")
        return 1
    print("OK #3787 sticky_last_limit_deny_all")
    return 0


if __name__ == "__main__":
    sys.exit(main())
