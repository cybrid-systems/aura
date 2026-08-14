#!/usr/bin/env python3
"""Issue #2990: ConcurrentMutationPolicy (SingleWriter vs ScopedParallel).

Workspace-layer policy. Complements #2976 AgentScope + #2985 health
(does not reimplement health throttle).

Contract:
  AC1 SingleWriter default; try_acquire no auto-redirect
  AC2 ScopedParallel opt-in; disjoint TLS region may redirect
  AC3 overlap fail-closed fallback SingleWriter (Soft; production is #3039)
  AC4 query:workspace-concurrency-stats schema-2990; #2985/#2976 wired
  AC5 extend test_workspace_region_concurrency; no docs/design / invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/workspace_concurrent_policy.hh")
    eix = _read("src/compiler/evaluator.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_workspace_region_concurrency.cpp")
    build = _read("build.py")

    # AC1 — SingleWriter default
    must("ConcurrentMutationPolicy", "AC1", hh)
    must("SingleWriter", "AC1", hh)
    must("ScopedParallel", "AC1", hh)
    must("resolve_concurrent_mutation_policy_default", "AC1", hh)
    must("kWorkspaceConcurrentPolicyIssue = 2990", "AC1", hh)
    must("scoped_parallel_enabled", "AC1", eix)
    must("set_concurrent_mutation_policy", "AC1", eix)
    must("scoped_parallel_enabled", "AC1 try_acquire", mb)
    must("#2990", "AC1", mb)

    # AC2 — ScopedParallel opt-in + redirect
    must("bump_scoped_parallel_redirect", "AC2", mb)
    must("workspace:set-concurrent-mutation-policy", "AC2", ws)
    must("ac2990_2_scoped_parallel_disjoint", "AC2", t)

    # AC3 — overlap fallback (Soft). Production hard-reject is #3039.
    must("bump_scoped_parallel_conflict_fallback", "AC3", mb)
    must("scoped_parallel_overlap_fallback", "AC3", mb)
    must("ac2990_3_overlap_fallback", "AC3", t)
    must("Issue #3039", "AC3 successor", mb)

    # AC4 — stats + no health reimplementation
    must_key("query:workspace-concurrency-stats", "AC4", q)
    must_key("schema-2990", "AC4", q)
    must("maybe_reject_mutation_concurrency_health", "AC4", mb)
    must("health-admit-wired", "AC4", q)
    must("agent-scope-policy-wired", "AC4", q)
    must("scoped_parallel_opt_in_total", "AC4", met)
    must("ac2990_4_stats_and_health", "AC4", t)

    # AC5 — suite + linter
    must("ac2990_1_single_writer_default", "AC5", t)
    must("ac2990_5_throughput_and_linter", "AC5", t)
    must("check_workspace_concurrent_policy_2990", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2990.cpp").is_file():
        fails.append("AC5: test_issue_2990.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "test_edsl_workspace_concurrent_policy.cpp").is_file():
        fails.append("AC5: test_edsl_workspace_concurrent_policy.cpp present (extend existing suite per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2990-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2990 ConcurrentMutationPolicy — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
