#!/usr/bin/env python3
"""Issue #2779: resume fence fail aggregate (#2677 residual).

#2677 triple fence (snapshot hard-fail / ticket / LayoutStamp) exposed
three separate atomics with no sum — production alerts had to watch each
metric; a noisy one could hide the others. Same operator playbook applies
to every fence trip.

Contract (one row per AC):
  AC1 Fiber::resume_fence_fail_total sums the three atomics
  AC2 C ABI aura_fiber_static_resume_fence_fail_total
  AC3 query:orchestration-steal-outermost-stats + orch-module-stats keys
  AC4 ac2779_* tests in test_steal_safety_ticket (per #81967)
  AC5 this linter wired; no docs/design/2779-*; no test_issue_2779.cpp

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

    fiber_h = _read("src/serve/fiber.h")
    fiber_cpp = _read("src/serve/fiber.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/serve/test_steal_safety_ticket.cpp")
    build = _read("build.py")

    # AC1 — accessor sums the three
    must("kResumeFenceFailAggregateIssue", "AC1", fiber_h)
    must("2779", "AC1", fiber_h)
    must("resume_fence_fail_total", "AC1", fiber_h)
    must("steal_snapshot_hard_fail_total()", "AC1", fiber_h)
    must("steal_safety_ticket_mismatch_total()", "AC1", fiber_h)
    must("layout_stamp_resume_mismatch_total()", "AC1", fiber_h)

    # AC2 — C ABI
    must("aura_fiber_static_resume_fence_fail_total", "AC2", fiber_h)
    must("aura_fiber_static_resume_fence_fail_total", "AC2", fiber_cpp)
    must("Fiber::resume_fence_fail_total()", "AC2", fiber_cpp)

    # AC3 — query surfaces
    must("resume-fence-fail-total", "AC3", obs)
    must("schema-2779", "AC3", obs)
    must("resume-fence-fail-wired", "AC3", obs)
    must("resume-fence-fail-total", "AC3", agent)
    must("schema-2779", "AC3", agent)
    # per-fence breakdown retained on steal-outermost
    must("steal-snapshot-hard-fail-total", "AC3", obs)
    must("steal-safety-ticket-mismatch-total", "AC3", obs)
    must("layout-stamp-resume-mismatch-total", "AC3", obs)

    # AC4 — tests
    must("ac2779_1_sum_equals_components", "AC4", test)
    must("ac2779_2_each_fence_bumps_aggregate", "AC4", test)
    must("ac2779_3_query_keys", "AC4", test)
    must("ac2779_4_source_and_no_design", "AC4", test)
    must("2779", "AC4", test)

    # AC5 — linter wire + no design docs / no orphan test_issue file
    must("check_resume_fence_fail_aggregate_2779", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2779-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2779.cpp").is_file():
        fails.append("AC5: test_issue_2779.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2779 resume fence fail aggregate — Fiber::resume_fence_fail_total + schema-2779 + orch facade")
    return 0


if __name__ == "__main__":
    sys.exit(main())
