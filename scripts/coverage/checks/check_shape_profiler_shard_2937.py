#!/usr/bin/env python3
"""Issue #2937: ShapeProfiler hot-path lock sharding for multi-fiber AI.

Contract (one row per AC):
  AC1 concurrent record_shape on distinct FnKeys uses per-shard locks
      (kShapeProfilerShardCount + shard_index + unique_lock_shard_)
  AC2 is_stable / dominant_shape / current_snapshot take shared_lock_shard_
  AC3 on_arena_compact still bans update_deopt_storm_state_ (#2617);
      does not bump mutation_induced_invalidations
  AC4 invalidate fires deopt hook after unlock; invalidate_unlocked_ retained
  AC5 Soft regression: concurrency suite extends #2141; compact isolation cited
  AC6 coverage linter + src/-aligned suite (#81967); no docs/design/

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


def _extract_fn_body(src: str, sig_pat: str) -> str | None:
    m = re.search(sig_pat, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    test = _read("tests/compiler/test_shape_profiler_concurrency.cpp")
    build = _read("build.py")

    # ── AC1: sharding ──
    must("#2937", "AC1", hh)
    must("kShapeProfilerShardIssue", "AC1", hh)
    must("kShapeProfilerShardCount", "AC1", hh)
    must("ProfileShard", "AC1", hh)
    must("shard_index", "AC1", hh)
    must("unique_lock_shard_", "AC1", cpp)
    must("unique_lock_all_shards_", "AC1", cpp)
    must("shared_lock_shard_", "AC1", cpp)
    must("shards_", "AC1", hh)
    must("kShapeProfilerShardIssue", "AC1 test", test)
    must("shard_index", "AC1 test", test)

    # ── AC2: readers use shared shard lock ──
    for name, sig in (
        ("is_stable", r"ShapeProfiler::is_stable\s*\("),
        ("dominant_shape", r"ShapeProfiler::dominant_shape\s*\("),
        ("current_snapshot", r"ShapeProfiler::current_snapshot\s*\("),
        ("metrics", r"ShapeProfiler::metrics\s*\("),
    ):
        body = _extract_fn_body(cpp, sig)
        if not body:
            fails.append(f"AC2: could not extract {name}")
            continue
        if "shared_lock_shard_" not in body:
            fails.append(f"AC2: {name} must use shared_lock_shard_")

    # ── AC3: compact isolation ──
    compact = _extract_fn_body(cpp, r"ShapeProfiler::on_arena_compact\s*\(")
    if not compact:
        fails.append("AC3: could not extract on_arena_compact")
    else:
        if "update_deopt_storm_state_" in compact.replace("Explicitly do NOT call update_deopt_storm_state_", ""):
            # Allow comment only
            stripped = re.sub(r"//[^\n]*", "", compact)
            if re.search(r"\bupdate_deopt_storm_state_\s*\(", stripped):
                fails.append("AC3: on_arena_compact must not call update_deopt_storm_state_")
        stripped = re.sub(r"//[^\n]*", "", compact)
        # Issue #3199: compact must not hold all shards at once.
        if re.search(r"\bunique_lock_all_shards_\s*\(", stripped):
            fails.append("AC3: on_arena_compact must not call unique_lock_all_shards_ (#3199)")
        if "unique_lock_shard_" not in compact:
            fails.append("AC3: on_arena_compact must lock per-shard (#3199)")
        must("Explicitly do NOT call update_deopt_storm_state_", "AC3", compact)
    must("#2617", "AC3", hh)

    # ── AC4: invalidate post-unlock hook ──
    inv = _extract_fn_body(cpp, r"ShapeProfiler::invalidate\s*\(\s*FnKey")
    if not inv:
        fails.append("AC4: could not extract invalidate")
    else:
        must("fire_shape_deopt_hook", "AC4", inv)
        must("unique_lock_shard_", "AC4", inv)
    must("invalidate_unlocked_", "AC4", cpp)

    # ── AC5: Soft / lineage ──
    must("#2141", "AC5", hh)
    must("lock_contended_total", "AC5", hh)
    must("run_test_shape_profiler_concurrency", "AC5", test)
    must("check_shape_compact_storm_isolation_2617", "AC5 lineage", build)

    # ── AC6: tests + build + no design ──
    must("check_shape_profiler_shard_2937", "AC6", build)
    must("cmd_shape_profiler_shard_2937", "AC6", build)
    must("Issue #2937", "AC6", test)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2937-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2937.cpp").is_file():
        fails.append("AC6: test_issue_2937.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2937 ShapeProfiler FnKey lock sharding — per-shard record_shape + #2617 compact isolation preserved"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
