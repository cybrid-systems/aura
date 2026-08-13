#!/usr/bin/env python3
"""Issue #2952: storm-clear + exhaust auto coverage-verify min-dirty.

Refine #2895/#2601/#2544 — production residual (force & ~last_success)
auto-seeds min-dirty + one #2601-gated reemit on storm clear / force drain.
Soft observe-only; env AURA_COVERAGE_VERIFY_MIN_DIRTY=0 opts out.

Contract (one row per AC):
  AC1  production + residual + storm clear → scheduled
  AC2  Soft / mask idle → no schedule (quiet)
  AC3  storm re-entry → storm-skip; force state retained
  AC4  #2601 cap/backoff + #2895 stamp + #2249 lineage preserved
  AC5  schema-2952 additive; #2544/#2601/#2895 preserved
  AC6  tests + build.py; no invent/design

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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_exhausted_min_dirty_reemit.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2952", "AC1", cpp)
    must("maybe_coverage_verify_min_dirty", "AC1", cpp)
    must("maybe_coverage_verify_min_dirty", "AC1", hh)
    must("resolve_coverage_verify_min_dirty_enabled", "AC1", cpp)
    must("2952 AC1", "AC1", test)

    # AC2
    must("AURA_COVERAGE_VERIFY_MIN_DIRTY", "AC2", cpp)
    must("AURA_SANDBOX", "AC2", cpp)
    must("aura_production_defaults_active_probe", "AC2", cpp)
    must("2952 AC2", "AC2", test)

    # AC3
    must("coverage_verify_storm_skip_total_", "AC3", hh)
    must("2952 AC3", "AC3", test)

    # AC4
    must("decide_exhausted_min_dirty_retry", "AC4", cpp)
    must("last_reemit_success_region_mask_", "AC4", cpp)
    must("on_exhausted_min_dirty_queue", "AC4", cpp)
    must("2952 AC4", "AC4", test)

    # AC5
    must("schema-2952", "AC5", mut)
    must("issue-2952", "AC5", mut)
    must("coverage-verify-scheduled-total", "AC5", mut)
    must("coverage-verify-success-total", "AC5", mut)
    must("coverage-verify-residual-uncovered-total", "AC5", mut)
    must("coverage-verify-storm-skip-total", "AC5", mut)
    must("coverage-verify-min-dirty-wired", "AC5", mut)
    must("schema-2601", "AC5", mut)
    must("schema-2895", "AC5", mut)
    must("schema_2952", "AC5", hh)

    # AC6
    must("2952", "AC6", test)
    must("check_coverage_verify_min_dirty_2952", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2952.cpp").is_file():
        fails.append("AC6: test_issue_2952.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2952-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Wire path: storm-clear + drain call coverage-verify
    if "maybe_coverage_verify_min_dirty()" not in cpp:
        fails.append("AC6: storm-clear/drain must call maybe_coverage_verify_min_dirty()")
    if "coverage_verify_scheduled_total_" not in hh:
        fails.append("AC6: counter members missing from header")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2952 coverage-verify min-dirty closed loop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
