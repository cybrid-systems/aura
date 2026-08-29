#!/usr/bin/env python3
"""Issue #3357: ShapeProfiler record_shape TLS histogram merge.

Shards (#2937) stop process-wide serialisation; hot-FnKey unique_lock on
record_shape still contends under multi-fiber AI eval of one function.
TLS last-shape sample / count coalesces same-fiber repeats and merges
into the shard under one unique_lock at kShapeTlsMergeBatch.

Contract:
  AC1  TLS bucket + merge batch; lock_contended soak in concurrency suite
  AC2  is_stable / dominant_shape remain shard-shared; no storm from TLS
  AC3  ≤1 observation unique_lock path (no merge atomic); env/member disable
  AC4  compact / invalidate / #2617/#3199 unchanged
  AC5  no std::function; no process-wide lock; linter AFTER #3271; no invent

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

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    test = _read("tests/compiler/test_shape_profiler_concurrency.cpp")
    build = _read("build.py")

    must("kShapeTlsRecordMergeIssue = 3357", "AC1 stamp", hh)
    must("kShapeTlsMergeBatch", "AC1 batch", hh)
    must("kShapeTlsRecordSlots", "AC1 slots", hh)
    must("thread_local", "AC1 TLS", cpp)
    must("record_shape_apply_locked_", "AC1 merge apply", cpp)
    must("tls_merge_batches_total", "AC1 merge counter", hh)
    must("ac3357_1_hot_fnkey_less_contention", "AC1 test", test)

    must("shared_lock_shard_", "AC2 is_stable shared", cpp)
    must("ac3357_2_stability_unchanged", "AC2 test", test)

    must("first observation this window", "AC3 ≤1 unique_lock", cpp)
    must("AURA_SHAPE_TLS_MERGE", "AC3 env disable", cpp)
    must("set_tls_merge_enabled", "AC3 member disable", hh)
    must("ac3357_3_soft_zero_extra", "AC3 test", test)

    must("ac3357_4_compact_invalidate_unchanged", "AC4 test", test)
    must("kShapeCompactNoAllShardsLockIssue = 3199", "AC4 #3199", hh)
    must("kShapeCompactStormIsolationIssue = 2617", "AC4 #2617", hh)

    if "std::function<" in hh:
        fails.append("AC5: shape_profiler.h has std::function<")
    if "std::function<" in cpp:
        fails.append("AC5: shape_profiler.cpp has std::function<")
    must("ac3357_5_source_and_linter", "AC5 test", test)
    must("check_shape_tls_record_merge_3357", "AC5 build.py", build)
    prev = build.find("check_shape_dirty_hook_no_std_function_3271")
    ours = build.find("check_shape_tls_record_merge_3357")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3271")
    if "schema-3357" in hh or "schema-3357" in cpp:
        fails.append("AC5: new schema-3357 query key")
    if "g_3357_" in hh or "g_3357_" in cpp:
        fails.append("AC5: new g_3357_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3357.cpp").is_file():
        fails.append("AC5: test_issue_3357.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3357.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3357.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3357-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3357 shape_tls_record_merge:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3357 shape_tls_record_merge: TLS coalesce + shard merge")
    return 0


if __name__ == "__main__":
    sys.exit(main())
