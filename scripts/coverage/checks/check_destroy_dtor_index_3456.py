#!/usr/bin/env python3
"""Issue #3456: ASTArena::destroy indexes ptr→dtors_ slot (swap-remove).

#1519 closed nullptr no-op + dtor+recycle+erase. Residual: destroy still
walked dtors_.begin() so Guard-scoped temps paid O(live objects).

Contract:
  AC1 create+destroy happy path uses dtor_index_ / swap-remove; linear
      for only on miss
  AC2 reset / ~ASTArena still runs remaining dtors exactly once
  AC3 Moving still sees size/align (#2166); #3420 refuse path unchanged
  AC4 no new query key
  AC5 no docs/design/3456-*; no test_issue_3456.cpp

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

    arena = _read("src/core/arena.ixx")
    t = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    l3420 = _read("scripts/coverage/checks/check_factory_refuse_uncovered_3420.py")
    build = _read("build.py")

    must("kDestroyDtorIndexIssue = 3456", "AC1 stamp", arena)
    must("dtor_index_", "AC1 index", arena)
    must("swap_remove_dtor_at_", "AC1 swap-remove", arena)
    must("note_dtor_entry_", "AC1 register", arena)
    must("rebuild_dtor_index_", "AC3 relocate rebuild", arena)

    dpos = arena.find("void destroy(T* ptr)")
    dwin = arena[dpos : dpos + 1800] if dpos >= 0 else ""
    must("dtor_index_.find(ptr)", "AC1 happy-path find", dwin)
    must("swap_remove_dtor_at_", "AC1 destroy swap", dwin)
    if "for (auto it = dtors_.begin(); it != dtors_.end(); ++it)" in dwin:
        fails.append("AC1: destroy still walks dtors_.begin() on the happy path")
    must("Issue #3456", "AC1 cite", dwin)

    must("run_destructors()", "AC2 dtor", arena)
    rpos = arena.find("void run_destructors() noexcept")
    rwin = arena[rpos : rpos + 900] if rpos >= 0 else ""
    must("dtors_.rbegin()", "AC2 reverse remaining", rwin)
    must("dtor_index_.clear()", "AC2 index clear", rwin)

    must("e.size", "AC3 Moving size", arena)
    must("e.align", "AC3 Moving align", arena)
    must("kFactoryRefuseUncoveredIssue = 3420", "AC3 #3420 stamp", arena)
    must("check_factory_refuse_uncovered_3420", "AC3 #3420 linter", l3420)

    must("ac3456_destroy_dtor_index", "AC1 test", t)
    must("check_destroy_dtor_index_3456", "AC5 build.py", build)
    if "schema-3456" in arena:
        fails.append("AC4: new schema-3456 query key")
    if (ROOT / "tests" / "core" / "test_issue_3456.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3456.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3456.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3456.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3456-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3456 destroy dtor_index_ swap-remove — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
