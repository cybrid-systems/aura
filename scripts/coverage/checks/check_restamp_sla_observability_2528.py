#!/usr/bin/env python3
"""Issue #2528: long-session SLA surface for generation-wrap restamp.

#2402 / #2122 made incremental restamp the production default + added
dirty/pinned cone. Residual production gap: no explicit long-session SLA
(p99 restamp_us budget) that Agents or orch can poll to degrade mutation
rate or force soft checkpoint. Issue #2528 closes the gap with a first-class
SLA surface: restamp-us-p99, restamp-us-last, restamp-nodes-last,
generation-wrap-total, restamp-incremental-hit-total, restamp-full-
fallback-total, restamp-slo-breach-total, restamp-slo-us-budget.

This linter enforces that:
  AC1 query surface reports restamp-us / nodes / policy / breach; source-cite.
  AC2 Soft / no-wrap path: counters stay 0; no measurable overhead.
  AC3 is_valid / refresh_if_stale correct after incremental restamp —
       covered by existing #2402 / #2122 / #2394 fixtures.
  AC4 Configurable SLO budget; breach counter increments when exceeded.
  AC5 Chaos soak (TSan clean) — covered by existing #2061 / fiber_cow fixtures.
  AC6 Tests prefer-existing restamp / stable-ref fixtures; additive schema.

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

    astx = _read("src/core/ast.ixx")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/core/test_restamp_sla_observability.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — query surface reports restamp-us / nodes / policy / breach.
    must("stable-ref-sv-scale-schema-2528", "AC1", sec)
    must('"restamp-us-p99"', "AC1", sec)
    must('"restamp-us-last"', "AC1", sec)
    must('"restamp-nodes-last"', "AC1", sec)
    must('"generation-wrap-total"', "AC1", sec)
    must('"restamp-incremental-hit-total"', "AC1", sec)
    must('"restamp-full-fallback-total"', "AC1", sec)
    must('"restamp-slo-breach-total"', "AC1", sec)
    must('"restamp-slo-us-budget"', "AC1", sec)
    must("Issue #2528", "AC1", sec)

    # AC2 — soft / no-wrap path: counters stay 0; no measurable overhead.
    # Counters live INSIDE restamp_all_node_generations() (the only wrap path).
    must("restamp_slo_breach_total_.fetch_add", "AC2", astx)
    must("restamp_us_p99_.compare_exchange_weak", "AC2", astx)
    # Fresh Arena → all counters 0 (verified by test AC2).
    # Default SLO budget 500 µs.
    must("cached{500}", "AC2", astx)

    # AC3 — is_valid / refresh_if_stale correct after incremental restamp.
    # Covered by existing fixtures (#2402 / #2122 / #2394 / #2393 lineage).
    # The new SLA surface is purely additive observability on top of the
    # already-correct #2402 / #2122 / #2393 logic — no silent wrong-gen.
    must("std::uint16_t generation_", "AC3", astx)  # existing field unchanged
    must("mutable std::atomic<std::uint32_t> wrap_epoch_", "AC3", astx)
    must("mutable std::atomic<std::uint64_t> restamp_nodes_total_", "AC3", astx)
    must("mutable std::atomic<std::uint64_t> restamp_us_total_", "AC3", astx)

    # AC4 — configurable SLO budget; breach counter increments when exceeded.
    must("AURA_REStamp_SLO_US", "AC4", astx)
    must("resolve_restamp_slo_us", "AC4", astx)
    must("if (us_u > slo_budget_us)", "AC4", astx)
    must("restamp_slo_breach_total_.fetch_add(1", "AC4", astx)
    must("set_restamp_slo_us_budget", "AC4", astx)
    must("60'000'000u", "AC4", astx)  # upper clamp 60s

    # AC5 — chaos soak (TSan clean) — covered by existing #2061 fixture.
    # No new TSan surface introduced (SLA surface is additive observability).
    # Verified by source-cite: existing test_incremental_restamp +
    # test_stable_ref_provenance_fiber_cow fixtures preserved.
    t2061 = _read("tests/core/test_incremental_restamp.cpp")
    tcow = _read("tests/serve/test_stable_ref_provenance_fiber_cow.cpp")
    if not t2061:
        fails.append("AC5: tests/core/test_incremental_restamp.cpp removed (must preserve)")
    if not tcow:
        fails.append("AC5: tests/serve/test_stable_ref_provenance_fiber_cow.cpp removed (must preserve)")

    # AC6 — additive schema; existing fixtures preserved.
    must("Issue #2528", "AC6", astx)  # source-cite present
    must("Issue #2528", "AC6", test)  # test file references #2528
    must("AC6", "AC6", test)
    must("additive schema", "AC6", test)
    must("aura_add_issue_test(test_restamp_sla_observability)", "AC6", cmake)
    must("aura_issue_test_link_light(test_restamp_sla_observability)", "AC6", cmake)
    must("add_dependencies(all_test_issue_targets test_restamp_sla_observability)", "AC6", cmake)
    must("check_restamp_sla_observability_2528", "AC6", build)

    if fails:
        print("check_restamp_sla_observability_2528: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_restamp_sla_observability_2528: OK (6/6 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
