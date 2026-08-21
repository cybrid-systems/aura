#!/usr/bin/env python3
"""Issue #3233: stale SafePCVSpan after Guard forces exclusive set_child.

A long-lived SafePCVSpan held across a successful structural Guard still
aliases PCV storage (use_count()>1) after the #3167 fingerprint is stale.
Production locked set_child_locked force-inplace exclusive (no full COW
alloc) for replace-one-child. Live non-stale spans and MutationCheckpoint
snapshots still COW. Soft unchanged. Reuses #2906 exclusive counters.

Contract:
  AC1 Stale span across Guard → next set_child exclusive, no COW alloc
  AC2 Live non-stale SafePCVSpan still COWs
  AC3 Checkpoint rollback still COWs
  AC4 Soft unchanged; exclusive/COW metrics; schema-3233
  AC5 Source-cite PCV/ast; extend existing suite; no invent / docs/design

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

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/core/test_pcv_exclusive_with_set.cpp")
    build = _read("build.py")

    must("kPcvStaleSpanExclusiveIssue = 3233", "AC1 stamp", hh)
    must("stale_span_force_exclusive_total", "AC1 metric", hh)
    must("force_inplace", "AC1 cow_set", hh)
    must("stale_exclusive", "AC1 locked", ast)
    must("stamp_last_pin_generation", "AC1 pin stamp", ast)
    must("ac3233_1_stale_span_next_set_child_exclusive", "AC1 test", t)

    must("ac3233_2_live_span_still_cows", "AC2 test", t)
    must("use_count()>1", "AC2 live COW", hh)

    must("pcv_checkpoint_live_enter", "AC3 checkpoint", hh)
    must("pcv_checkpoint_live_enter", "AC3 boundary", mb)
    must("ac3233_3_checkpoint_rollback_still_cows", "AC3 test", t)

    must("stale_span_force_exclusive_enabled", "AC4 enable", hh)
    must("schema-3233", "AC4 schema", obs)
    must("schema-2906", "AC4 2906 preserved", obs)
    must("ac3233_4_soft_unchanged_source", "AC4 test", t)

    must("Issue #3233", "AC5 ast", ast)
    must("Issue #3233", "AC5 pcv", hh)
    must("check_pcv_stale_span_exclusive_3233", "AC5 build.py", build)
    must("kPcvSpanStaleAcrossGuardIssue = 3167", "AC5 3167 lineage", hh)

    if "g_3233_" in hh:
        fails.append("AC5: new g_3233_* counter")
    if (ROOT / "tests" / "core" / "test_issue_3233.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3233.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3233.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3233.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3233-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3233 pcv_stale_span_exclusive:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3233 pcv_stale_span_exclusive: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
