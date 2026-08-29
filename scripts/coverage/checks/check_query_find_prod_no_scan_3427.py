#!/usr/bin/env python3
"""Issue #3427: production query:find miss is not a full SoA walk.

#3390 routed Define names through find_define_by_name before the scan.
Production miss / non-Define still walked flat.size() + get(id). That
is the I6 multi-round cliff. Production miss is now query-unindexed
(reuse overflow counter). Soft keeps the size() first-match walk.

Contract:
  AC1 production Define-name hit unchanged (#3390)
  AC2 production miss → query-unindexed; production branch has zero
      id < flat.size() loops / flat.get(id) in a size() loop
  AC3 Soft size() fallback + first-match unchanged
  AC4 tests in test_query_find_by_define; linter after #3395;
      no docs/design/3427-*; no test_issue_3427.cpp
  AC5 no new Agent-facing query name beyond the error kind

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

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    t = _read("tests/compiler/test_query_find_by_define.cpp")
    build = _read("build.py")

    start = qws.find('["query:find"]')
    nxt = qws.find('["query:children"]', start) if start >= 0 else -1
    win = qws[start:nxt] if start >= 0 and nxt > start else ""
    fd = win.find("find_define_by_name")
    after = win[fd:] if fd >= 0 else ""
    soft = after.find("Fallback: existing size() walk")
    prod = after[:soft] if soft >= 0 else after

    must("find_define_by_name", "AC1 index", win)
    must("Issue #3390", "AC1 #3390 cite", win)
    must("test_query_find_by_define", "AC1 test file", t)

    must("Issue #3427", "AC2 cite", prod)
    must("query-unindexed", "AC2 error kind", prod)
    must("production_defaults_active()", "AC2 production gate", prod)
    must("note_query_result_overflow_total", "AC2 reuse overflow counter", prod)
    if "id < flat.size()" in prod:
        fails.append("AC2: production branch of query:find contains id < flat.size()")
    if "flat.get(id)" in prod:
        fails.append("AC2: production branch of query:find calls flat.get(id)")
    must("test_query_find_by_define", "AC2 test", t)
    must("query-unindexed", "AC2 test kind", t)

    must("id < flat.size()", "AC3 Soft size() fallback", after)
    must("try_acquire_soa_reader_lock", "AC3 Soft SoA lock", after)
    must("Issue #2488", "AC3 #2488 cite", after)

    must("check_query_find_prod_no_scan_3427", "AC4 build.py", build)
    prev = build.find("check_query_default_stamped_3395")
    ours = build.find("check_query_find_prod_no_scan_3427")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: #3427 linter must run after #3395")
    if (ROOT / "tests" / "issues" / "test_issue_3427.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3427.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3427.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3427.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3427-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if 'add("query:unindexed' in qws or 'add("query:find-unindexed' in qws:
        fails.append("AC5: new Agent-facing query name")
    if "g_3427_" in qws:
        fails.append("AC5: new g_3427_* counter")
    if "schema-3427" in qws:
        fails.append("AC5: new schema-3427 query key")

    if fails:
        print("FAIL #3427 query_find_prod_no_scan:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3427 query_find_prod_no_scan: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
