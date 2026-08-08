#!/usr/bin/env python3
"""Issue #2790: atomic-batch sub-op failure sets guard_ok (not only ok).

A sub-op bool/#f or unexpected failure that only set ok=false left
guard_ok true; MutationBoundaryGuard RAII then committed the prefix.

Contract (one row per AC):
  AC1 atomic-batch body cites #2790; mark_sub_op_failed sets ok+guard_ok
  AC2 bool-false / !sub_result paths call mark_sub_op_failed
  AC3 tests/compiler/test_atomic_batch_partial_failure.cpp + no test_issue_2790.cpp
  AC4 this linter wired; no docs/design/2790-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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
    test = _read("tests/compiler/test_atomic_batch_partial_failure.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add("mutate:atomic-batch"')
    if pos < 0:
        pos = mut.find("mutate:atomic-batch")
    if pos < 0:
        fails.append("AC1: mutate:atomic-batch not found")
        win = ""
    else:
        end = mut.find("typed-mutate-atomic", pos)
        if end < 0:
            end = pos + 8000
        win = mut[pos:end]

    # AC1
    must("Issue #2790", "AC1", win)
    must("mark_sub_op_failed", "AC1", win)
    # Helper body
    m = re.search(
        r"mark_sub_op_failed\s*=\s*\[&\]\s*\(\)\s*\{[^}]*ok\s*=\s*false[^}]*guard_ok\s*=\s*false",
        win,
        re.DOTALL,
    )
    if not m:
        fails.append("AC1: mark_sub_op_failed must set both ok and guard_ok")

    # AC2 — failure call sites
    if win.count("mark_sub_op_failed()") < 3:
        fails.append(f"AC2: expected >=3 mark_sub_op_failed() calls, found {win.count('mark_sub_op_failed()')}")
    # Forbid ok-only break immediately after bool-false check without helper.
    if re.search(
        r"is_bool\(\*sub_result\).*?as_bool\(\*sub_result\)\s*\)\s*\{\s*ok\s*=\s*false\s*;\s*break",
        win,
        re.DOTALL,
    ):
        fails.append("AC2: residual ok-only bool-false break (missing mark_sub_op_failed)")

    # AC3
    must("ac2790", "AC3", test)
    must("2790", "AC3", test)
    must("mark_sub_op_failed", "AC3", test)
    must("batch-failed", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_atomic_batch_partial_failure.cpp").is_file():
        fails.append("AC3: missing test_atomic_batch_partial_failure.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2790.cpp").is_file():
        fails.append("AC3: test_issue_2790.cpp present (forbidden per #81967)")
    must("test_atomic_batch_partial_failure", "AC3", cmake)

    # AC4
    must("check_atomic_batch_partial_failure_2790", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2790-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2790 atomic-batch partial failure — mark_sub_op_failed sets ok+guard_ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
