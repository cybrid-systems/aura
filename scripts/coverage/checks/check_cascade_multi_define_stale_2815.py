#!/usr/bin/env python3
"""Issue #2815: cascade marks all Define NodeIds for a name (not first-wins).

Contract (one row per AC):
  AC1 cascade uses affected_defs by NodeId; cites #2815; multi metric
  AC2 path2 notes ALL Defines with name; metric in metrics.h + query
  AC3 test suite present
  AC4 this linter wired; no docs/design/2815-*; no test_issue_2815.cpp

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
    met = _read("src/compiler/observability_metrics.h")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_cascade_multi_define_stale.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    cascade = mut.find("push_post_mutate_incremental_cascade")
    end = mut.find("// 3) Eager partial re-lower", cascade if cascade >= 0 else 0)
    if end < 0:
        end = (cascade if cascade >= 0 else 0) + 9000
    win = mut[cascade:end] if cascade >= 0 else ""

    # AC1
    must("Issue #2815", "AC1", win)
    must("affected_defs", "AC1", win)
    must("cascade_multi_define_stale_total", "AC1", win)
    must("multi_define_extra", "AC1", win)

    # AC2
    must("ALL Define", "AC2", win)
    must("cascade_multi_define_stale_total", "AC2", met)
    must("schema-2815", "AC2", obs)
    must("cascade_multi_define_stale_total", "AC2", obs)

    # AC3
    must("ac2815", "AC3", test)
    must("2815", "AC3", test)
    must("affected_defs", "AC3", test)
    must("cascade_multi_define_stale_total", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_cascade_multi_define_stale.cpp").is_file():
        fails.append("AC3: missing test_cascade_multi_define_stale.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2815.cpp").is_file():
        fails.append("AC3: test_issue_2815.cpp present (forbidden per #81967)")
    must("test_cascade_multi_define_stale", "AC3", cmake)

    # AC4
    must("check_cascade_multi_define_stale_2815", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2815-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2815 cascade multi-define — all Define NodeIds marked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
