#!/usr/bin/env python3
"""Issue #2796: atomic-batch abort must not call linear_post_mutate_enforce_all.

Post-rollback enforce pollutes linear_invariant_fail /
invariant_violations_caught with rollback noise.

Contract (one row per AC):
  AC1 abort_batch_workspace cites #2796; no live enforce_all call in helper
  AC2 all three abort paths use abort_batch_workspace
  AC3 tests/compiler/test_atomic_batch_rollback_metric_noise.cpp + no test_issue_2796.cpp
  AC4 this linter wired; no docs/design/2796-*

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
    test = _read("tests/compiler/test_atomic_batch_rollback_metric_noise.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add_mutate("mutate:atomic-batch"')
    if pos < 0:
        pos = mut.find("mutate:atomic-batch")
    win = mut[pos : pos + 22000] if pos >= 0 else ""

    # AC1
    must("Issue #2796", "AC1", win)
    must("abort_batch_workspace", "AC1", win)
    hpos = win.find("auto abort_batch_workspace")
    if hpos < 0:
        fails.append("AC1: abort_batch_workspace lambda missing")
        hbody = ""
    else:
        hend = win.find("while (is_pair(op_list))", hpos)
        if hend < 0:
            hend = hpos + 900
        hbody = win[hpos:hend]
    # Live call pattern (not comment): (void)ev.linear_post_mutate_enforce_all
    if re.search(r"\(void\)\s*ev\.linear_post_mutate_enforce_all\s*\(", hbody):
        fails.append("AC1: live linear_post_mutate_enforce_all in abort helper")
    # Whole atomic-batch body should have no live enforce_all calls.
    # Strip // comments then search.
    stripped = re.sub(r"//[^\n]*", "", win)
    if re.search(r"\(void\)\s*ev\.linear_post_mutate_enforce_all\s*\(", stripped):
        fails.append("AC1: live (void)ev.linear_post_mutate_enforce_all still in atomic-batch")

    # AC2: three call sites (unsupported, throw, !ok)
    calls = win.count("abort_batch_workspace()")
    if calls < 3:
        fails.append(f"AC2: expected >=3 abort_batch_workspace() uses, got {calls}")

    # AC3
    must("ac2796", "AC3", test)
    must("2796", "AC3", test)
    must("linear_invariant_fail", "AC3", test)
    must("abort_batch_workspace", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_atomic_batch_rollback_metric_noise.cpp").is_file():
        fails.append("AC3: missing test_atomic_batch_rollback_metric_noise.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2796.cpp").is_file():
        fails.append("AC3: test_issue_2796.cpp present (forbidden per #81967)")
    must("test_atomic_batch_rollback_metric_noise", "AC3", cmake)

    # AC4
    must("check_atomic_batch_rollback_metric_noise_2796", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2796-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2796 atomic-batch rollback metric noise — no linear_post_mutate_enforce_all on abort")
    return 0


if __name__ == "__main__":
    sys.exit(main())
