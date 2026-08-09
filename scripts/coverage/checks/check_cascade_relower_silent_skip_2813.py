#!/usr/bin/env python3
"""Issue #2813: cascade must not silently skip ir_cache_v2 re-lower when fn null.

Contract (one row per AC):
  AC1 cascade cites #2813; cascade_relower_skipped/ran metrics + warn
  AC2 set_relower docs + CompilerService wire cites #2813
  AC3 test + query schema-2813
  AC4 this linter wired; no docs/design/2813-*; no test_issue_2813.cpp

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
    ixx = _read("src/compiler/evaluator.ixx")
    svc = _read("src/compiler/service.ixx")
    met = _read("src/compiler/observability_metrics.h")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_cascade_relower_silent_skip.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    cascade = mut.find("push_post_mutate_incremental_cascade")
    # Window must cover #2813 re-lower block (after #2815 multi-define expand).
    win = mut[cascade : cascade + 8000] if cascade >= 0 else ""
    # Prefer the dedicated #2813 block if present (more stable than fixed length).
    p2813 = mut.find("Issue #2813", cascade if cascade >= 0 else 0)
    if p2813 >= 0:
        win = mut[p2813 : p2813 + 2500]

    # AC1
    must("Issue #2813", "AC1", win)
    must("cascade_relower_skipped_total", "AC1", win)
    must("cascade_relower_ran_total", "AC1", win)
    must("relower_dirty_defines_fn_", "AC1", win)
    must("defines_n > 0", "AC1", win)
    # Must still call the fn when wired (not only metric).
    must("relower_dirty_defines_fn_()", "AC1", win)

    # AC2
    must("Issue #2813", "AC2", ixx)
    must("relower_dirty_defines_wired", "AC2", ixx)
    must("#2813", "AC2", svc)
    must("set_relower_dirty_defines_fn", "AC2", svc)
    must("cascade_relower_skipped_total", "AC2", met)
    must("cascade_relower_ran_total", "AC2", met)

    # AC3
    must("schema-2813", "AC3", obs)
    must("cascade_relower_skipped_total", "AC3", obs)
    must("ac2813", "AC3", test)
    must("2813", "AC3", test)
    must("cascade_relower_skipped_total", "AC3", test)
    must("set_relower_dirty_defines_fn", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_cascade_relower_silent_skip.cpp").is_file():
        fails.append("AC3: missing test_cascade_relower_silent_skip.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2813.cpp").is_file():
        fails.append("AC3: test_issue_2813.cpp present (forbidden per #81967)")
    must("test_cascade_relower_silent_skip", "AC3", cmake)

    # AC4
    must("check_cascade_relower_silent_skip_2813", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2813-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2813 cascade relower silent skip — metric + warn when unwired")
    return 0


if __name__ == "__main__":
    sys.exit(main())
