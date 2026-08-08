#!/usr/bin/env python3
"""Issue #2812: post-mutate cascade BFS invalidate after Guard unlock.

Contract (one row per AC):
  AC1 cascade enqueues precise defines under Guard (run_full=false retained)
  AC2 Guard dtor drains after unlock; clear on failure
  AC3 metrics + query schema-2812 + test suite
  AC4 this linter wired; no docs/design/2812-*; no test_issue_2812.cpp

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
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    met = _read("src/compiler/observability_metrics.h")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_cascade_bfs_invalidate_after_guard.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2812", "AC1", mut)
    must("enqueue_cascade_bfs_invalidate", "AC1", mut)
    must("define_needs_precise_invalidation", "AC1", mut)
    cascade = mut.find("push_post_mutate_incremental_cascade")
    cwin = mut[cascade : cascade + 4500] if cascade >= 0 else ""
    must("enqueue_cascade_bfs_invalidate", "AC1", cwin)
    if "run_full=*/false" not in cwin and "/*run_full=*/false" not in cwin:
        fails.append("AC1: soft finalize (run_full=false) must remain under Guard")

    # AC2
    must("Issue #2812", "AC2", bound)
    must("drain_cascade_bfs_invalidate", "AC2", bound)
    must("clear_cascade_bfs_invalidate", "AC2", bound)
    unlock = bound.find("lock_.unlock()")
    drain = bound.find("drain_cascade_bfs_invalidate")
    if unlock < 0 or drain < 0 or unlock >= drain:
        fails.append("AC2: drain_cascade_bfs_invalidate must appear after lock_.unlock()")
    must("drain_cascade_bfs_invalidate", "AC2", ixx)
    must("pending_cascade_bfs_invalidate_", "AC2", ixx)

    # AC3
    must("cascade_bfs_invalidate_total", "AC3", met)
    must("cascade_bfs_invalidate_pending_total", "AC3", met)
    must("cascade_bfs_invalidate_cleared_total", "AC3", met)
    must("schema-2812", "AC3", obs)
    must("cascade_bfs_invalidate_total", "AC3", obs)
    must("ac2812", "AC3", test)
    must("2812", "AC3", test)
    must("drain_cascade_bfs_invalidate", "AC3", test)
    must("enqueue_cascade_bfs_invalidate", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_cascade_bfs_invalidate_after_guard.cpp").is_file():
        fails.append("AC3: missing test_cascade_bfs_invalidate_after_guard.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2812.cpp").is_file():
        fails.append("AC3: test_issue_2812.cpp present (forbidden per #81967)")
    must("test_cascade_bfs_invalidate_after_guard", "AC3", cmake)

    # AC4
    must("check_cascade_bfs_invalidate_after_guard_2812", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2812-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2812 cascade BFS invalidate after Guard unlock — closures coherent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
