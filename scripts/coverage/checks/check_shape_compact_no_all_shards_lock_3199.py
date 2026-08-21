#!/usr/bin/env python3
"""Issue #3199: on_arena_compact must not unique_lock_all_shards_.

#2937 sharded record_shape. Residual: compact still took all 16 unique
locks at once, re-serializing disjoint FnKey traffic under densify/compact.

Contract:
  AC1 compact body never calls unique_lock_all_shards_; per-shard unique only
  AC2 per-profile version still advances (LayoutStamp / SpecJIT see it)
  AC3 #2617 isolation: no update_deopt_storm_state_ / mutation_induced bump
  AC4 Soft / no-compact unchanged; concurrency + compact isolation suites
  AC5 this linter + build.py; no new public query key / g_3199_*
  AC6 no docs/design/3199-* / test_issue_3199.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


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
    conc = _read("tests/compiler/test_shape_profiler_concurrency.cpp")
    iso = _read("tests/compiler/test_shape_compact_storm_isolation.cpp")
    build = _read("build.py")
    shard_lint = _read("scripts/coverage/checks/check_shape_profiler_shard_2937.py")

    must("kShapeCompactNoAllShardsLockIssue = 3199", "AC1 stamp", hh)
    must("Issue #3199", "AC1 header", hh)
    must("Issue #3199", "AC1 cpp", cpp)

    compact = _extract_fn_body(cpp, r"ShapeProfiler::on_arena_compact\s*\(\s*\)\s*noexcept")
    if not compact:
        fails.append("AC1: could not extract on_arena_compact")
    else:
        stripped = re.sub(r"//[^\n]*", "", compact)
        if re.search(r"\bunique_lock_all_shards_\s*\(", stripped):
            fails.append("AC1: on_arena_compact calls unique_lock_all_shards_")
        if "unique_lock_shard_" not in compact:
            fails.append("AC1: on_arena_compact missing unique_lock_shard_")
        must("profile.version++", "AC2 version bump", compact)
        if re.search(r"\bupdate_deopt_storm_state_\s*\(", stripped):
            fails.append("AC3: on_arena_compact calls update_deopt_storm_state_")
        if re.search(r"mutation_induced_invalidations_\.fetch_add", stripped):
            fails.append("AC3: on_arena_compact bumps mutation_induced_invalidations_")
        must("Explicitly do NOT call update_deopt_storm_state_", "AC3", compact)

    must("unique_lock_all_shards_", "AC4 helper retained", cpp)
    must("ac3199_1_compact_no_all_shards", "AC1 test", conc)
    must("ac3199_2_version_advances", "AC2 test", conc)
    must("ac3199_3_compact_not_storm", "AC3 test", conc)
    must("3199", "AC4 compact isolation suite", iso)
    must("check_shape_compact_no_all_shards_lock_3199", "AC5 build.py", build)
    must("#3199", "AC5 2937 linter lineage", shard_lint)

    if "g_3199_" in hh or "g_3199_" in cpp:
        fails.append("AC5: invented g_3199_* counter")
    if "query:shape-compact-no-all-shards" in cpp:
        fails.append("AC5: new public query key")

    must("ac3199_4_source_and_linter", "AC6 test", conc)
    if (ROOT / "tests" / "compiler" / "test_issue_3199.cpp").is_file():
        fails.append("AC6: test_issue_3199.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3199.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3199.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3199-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3199 on_arena_compact per-shard lock — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
