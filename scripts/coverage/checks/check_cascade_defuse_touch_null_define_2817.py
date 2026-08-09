#!/usr/bin/env python3
"""Issue #2817: cascade skips defuse_touch for ghost names (no live Define).

Contract (one row per AC):
  AC1 cascade cites #2817; names_with_def filter; ghost metric
  AC2 metrics.h + query schema-2817
  AC3 test suite present
  AC4 this linter wired; no docs/design/2817-*; no test_issue_2817.cpp

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
    test = _read("tests/compiler/test_cascade_defuse_touch_null_define.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    cascade = mut.find("push_post_mutate_incremental_cascade")
    end = mut.find("// 3) Eager partial re-lower", cascade if cascade >= 0 else 0)
    if end < 0:
        end = (cascade if cascade >= 0 else 0) + 9000
    body = mut[cascade:end] if cascade >= 0 else ""

    # AC1
    must("Issue #2817", "AC1", body)
    must("cascade_ghost_name_touch_total", "AC1", body)
    must("names_with_def", "AC1", body)
    must("defuse_touch_fn_", "AC1", body)
    must("ghost_name_touches", "AC1", body)

    # AC2
    must("cascade_ghost_name_touch_total", "AC2", met)
    must("schema-2817", "AC2", obs)
    must("cascade_ghost_name_touch_total", "AC2", obs)

    # AC3
    must("ac2817", "AC3", test)
    must("2817", "AC3", test)
    must("cascade_ghost_name_touch_total", "AC3", test)
    must("ghost", "AC3", test)
    must("propagate_defuse_dirty", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_cascade_defuse_touch_null_define.cpp").is_file():
        fails.append("AC3: missing test_cascade_defuse_touch_null_define.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2817.cpp").is_file():
        fails.append("AC3: test_issue_2817.cpp present (forbidden per #81967)")
    must("test_cascade_defuse_touch_null_define", "AC3", cmake)

    # AC4
    must("check_cascade_defuse_touch_null_define_2817", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2817-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2817 cascade ghost-name defuse_touch skip — no stale index")
    return 0


if __name__ == "__main__":
    sys.exit(main())
