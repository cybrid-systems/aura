#!/usr/bin/env python3
"""Issue #2791: mutate:rebind parse-error frees orphan parse appends.

parse_to_flat appends into the workspace flat; Guard rollback only
processes mutation-log records. On parse-error, free_orphan_nodes_from
must reclaim the [size_before_parse, size()) range.

Contract (one row per AC):
  AC1 rebind body cites #2791 + free_orphan_nodes_from / free_rebind_parse_orphans
  AC2 set-body parse-error also frees orphans (#2791 parity)
  AC3 tests/compiler/test_rebind_parse_failure_no_leak.cpp + no test_issue_2791.cpp
  AC4 this linter wired; no docs/design/2791-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_rebind_parse_failure_no_leak.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    rpos = mut.find('add_mutate("mutate:rebind"')
    if rpos < 0:
        rpos = mut.find("mutate:rebind")
    if rpos < 0:
        fails.append("AC1: mutate:rebind not found")
        rwin = ""
    else:
        rend = mut.find('add_mutate("mutate:set-body"', rpos)
        if rend < 0:
            rend = rpos + 7000
        rwin = mut[rpos:rend]

    # AC1
    must("Issue #2791", "AC1", rwin)
    must("free_orphan_nodes_from", "AC1", rwin)
    must("free_rebind_parse_orphans", "AC1", rwin)
    must("size_before_parse", "AC1", rwin)

    # AC2 — set-body
    spos = mut.find('add_mutate("mutate:set-body"')
    if spos < 0:
        spos = mut.find("mutate:set-body")
    if spos < 0:
        fails.append("AC2: mutate:set-body not found")
        swin = ""
    else:
        send = mut.find("add_mutate(", spos + 20)
        if send < 0:
            send = spos + 5000
        swin = mut[spos:send]
    must("Issue #2791", "AC2", swin)
    must("free_orphan_nodes_from", "AC2", swin)
    must("free_set_body_parse_orphans", "AC2", swin)

    # AC3
    must("ac2791", "AC3", test)
    must("2791", "AC3", test)
    must("free_orphan_nodes_from", "AC3", test)
    must("live_node_count", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_rebind_parse_failure_no_leak.cpp").is_file():
        fails.append("AC3: missing test_rebind_parse_failure_no_leak.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2791.cpp").is_file():
        fails.append("AC3: test_issue_2791.cpp present (forbidden per #81967)")
    must("test_rebind_parse_failure_no_leak", "AC3", cmake)

    # AC4
    must("check_rebind_parse_failure_no_leak_2791", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2791-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2791 rebind parse-error free orphans — free_orphan_nodes_from + set-body parity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
