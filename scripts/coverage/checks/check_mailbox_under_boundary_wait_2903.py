#!/usr/bin/env python3
"""Issue #2903: deferred-under-boundary wait histogram + Agent-visible metrics.

Contract:
  AC1 wait sample helper + hist/max/p50/p99; sample on Ok window close + budget drop
  AC2 zero-cost no-defer path (depth==0 short-circuit retained)
  AC3 schema-2903 additive query keys; #2849/#2511/#2378 preserved
  AC4 ac2903_* tests (long vs short hold chaos-lite)
  AC5 source-cite + build.py wire; no docs/design/*

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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    health = _read("src/compiler/mutation_concurrency_health.hh")
    query = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # AC1 — wait sample + hist + wire sites
    must("Issue #2903", "AC1", mb)
    must("note_mailbox_under_boundary_wait_sample", "AC1", mb)
    must("mailbox_under_boundary_wait_us_total", "AC1", mb)
    must("mailbox_under_boundary_wait_samples", "AC1", mb)
    must("mailbox_under_boundary_wait_us_max", "AC1", mb)
    must("mailbox_under_boundary_wait_us_p50", "AC1", mb)
    must("mailbox_under_boundary_wait_us_p99", "AC1", mb)
    must("mailbox_under_boundary_wait_hist", "AC1", mb)
    must("kUnderBoundaryWaitHistBuckets", "AC1", mb)
    must("mailbox_under_boundary_wait_drop_total", "AC1", mb)
    # Sampled on Ok window close and budget force-drop.
    must("note_mailbox_under_boundary_wait_sample(wait_us", "AC1 Ok path", mb)
    must("/*dropped=*/true", "AC1 drop path", mb)

    # AC2 — zero cost no-defer
    must("if (depth == 0)", "AC2", mb)
    must("happy path — zero extra work beyond this load", "AC2", mb)
    must("never from the happy Ok path", "AC2", mb)

    # AC3 — schema + lineage preserved
    must("schema-2903", "AC3", msg)
    must("mailbox-under-boundary-wait-us-p50", "AC3", msg)
    must("mailbox-under-boundary-wait-us-p99", "AC3", msg)
    must("mailbox-under-boundary-wait-us-max", "AC3", msg)
    must("mailbox-under-boundary-wait-wired", "AC3", msg)
    must("mailbox-under-boundary-wait-hist-lt-100us", "AC3", msg)
    must("mailbox-under-boundary-wait-hist-ge-100ms", "AC3", msg)
    must("schema-2849", "AC3 lineage", msg)
    must("schema-2511", "AC3 lineage", msg)
    must("schema-2378", "AC3 lineage", msg)
    must("note_mailbox_deferred_under_boundary", "AC3 #2849", mb)
    must("mailbox_deferred_flush_latency_us_total", "AC3 #2378", mb)
    must("drain_deferred_under_budget", "AC3 #2511", mb)
    must("mailbox_under_boundary_wait_us_max", "AC3 health", health)
    must("component-mailbox-under-boundary-wait-us-max", "AC3 health query", query)

    # AC4 — tests
    must("ac2903_1_wait_recorded_after_exit_deliver", "AC4", test)
    must("ac2903_2_no_defer_zero_extra", "AC4", test)
    must("ac2903_3_schema_query_additive", "AC4", test)
    must("ac2903_4_chaos_lite_long_vs_short_hold", "AC4", test)
    must("ac2903_5_source_cite_no_docs_design", "AC4", test)
    must("Issue #2903", "AC4", test)

    # AC5 — build wire + no design docs
    must("check_mailbox_under_boundary_wait_2903", "AC5", build)
    must("cmd_mailbox_under_boundary_wait_2903", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2903-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2903.cpp").is_file():
        fails.append("tests/serve/test_issue_2903.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2903 mailbox under-boundary wait histogram — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
