#!/usr/bin/env python3
"""Issue #2824: run_dirty_pipeline TLS attribution isolates concurrent counters.

Contract (one row per AC):
  AC1 enter/leave TLS; contamination metric; #2824 cites
  AC2 multi-pass flush from TLS (not per-pass process deltas)
  AC3 test concurrent isolation
  AC4 linter wired; schema-2824; no docs/design/2824-*; no test_issue_2824.cpp

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

    core = _read("src/compiler/pass_pipeline_core.ixx")
    stats = _read("src/compiler/jit_typed_mutation_stats.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_dirty_pipeline_counter_isolation.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Prefer definition (after template requires), include preceding #2824 doc.
    pos = core.find("bool run_dirty_pipeline(IRModuleV2")
    if pos < 0:
        pos = core.find("bool run_dirty_pipeline")
    # Back up to capture the Issue #2824 comment block above the template.
    start = max(0, pos - 400) if pos >= 0 else 0
    body = core[start : (pos if pos >= 0 else 0) + 3500]

    # AC1
    must("Issue #2824", "AC1", body)
    must("enter_dirty_pipeline_attribution", "AC1", body)
    must("leave_dirty_pipeline_attribution", "AC1", body)
    must("pipeline_dirty_counter_concurrent_contamination_total", "AC1", body)
    must("enter_dirty_pipeline_attribution", "AC1", stats)
    must("g_dirty_pipeline_tls_depth", "AC1", stats)
    must("g_tls_dirty_pipeline_skips", "AC1", stats)
    must("Issue #2824", "AC1", stats)

    # AC2: no per-pass mig_skips0 delta aggregation (TLS flush only)
    if "mig_skips0" in body and "enter_dirty_pipeline_attribution" not in body:
        fails.append("AC2: residual per-pass mig_skips0 without TLS enter")
    must("tls_skips", "AC2", body)
    must("tls_runs", "AC2", body)

    # AC3
    must("ac2824", "AC3", test)
    must("2824", "AC3", test)
    must("dirty_block_driven_skips.fetch_add", "AC3", test)
    must("record_dirty_block_skip", "AC3", test)
    must("pipeline_dirty_counter_concurrent_contamination_total", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_dirty_pipeline_counter_isolation.cpp").is_file():
        fails.append("AC3: missing test_dirty_pipeline_counter_isolation.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2824.cpp").is_file():
        fails.append("AC3: test_issue_2824.cpp present (forbidden per #81967)")
    must("test_dirty_pipeline_counter_isolation", "AC3", cmake)

    # AC4
    must("check_dirty_pipeline_counter_isolation_2824", "AC4", build)
    must("schema-2824", "AC4", obs)
    must("pipeline-dirty-counter-concurrent-contamination-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2824-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2824 dirty pipeline counter isolation — TLS attribution")
    return 0


if __name__ == "__main__":
    sys.exit(main())
