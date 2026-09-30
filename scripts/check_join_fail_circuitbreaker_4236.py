#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4236: production CircuitBreaker compose left Scope
# on_join_fail=ReportOnly. to_agent_policy(FailurePolicy::CircuitBreaker) /
# compose_workflow_policy(CircuitBreaker) armed on_stall=Cancel +
# consecutive_stall_limit but never wrote on_join_fail (#4114 AC4 pinned
# the observe-first face), and apply_workflow routes to
# compose_supervised_batch only when on_join_fail != ReportOnly — a batch
# Timeout / QuotaExceeded residual did NOT cancel/join live Scope agents
# while the same host composing FailFast (#4114) did: juxtaposed APIs.
# Fix: the #2539 bridge arms on_join_fail=Cancel for CircuitBreaker under
# production defaults (mirror #4114 FailFast; Soft / Off keep ReportOnly,
# zero-cost); an explicit post-compose ReportOnly still wins (#3969 AC1
# face); RetryN / CollectAll mappings unchanged.
#
# AC1 - the bridge arms the join face: the CircuitBreaker case in
#       to_agent_policy writes on_join_fail=AgentFailureAction::Cancel
#       gated on production_defaults_active() (on_stall=Cancel +
#       consecutive_stall_limit threading unchanged); the mapping-table
#       comment documents the #4236 production arm; the #4236 stamp
#       constant exists.
# AC2 - explicit override wins: apply_workflow keeps the #3969 route gate
#       (production + on_join_fail != ReportOnly), so an explicit
#       post-compose ReportOnly stays observe-first; the test file pins
#       the explicit-ReportOnly runtime AC and the #3969 AC1 fixture.
# AC3 - production compose routes CircuitBreaker through
#       compose_supervised_batch, whose batch-fail arm joins under
#       to_agent_policy(w) (Cancel now armed by AC1); the runtime oracle
#       AC (circuit-open batch residual reaches the live Scope agent via
#       request_cancel / join_fail action taken >= 1) is pinned in the
#       bridge test; the cancel face reuses
#       agent_join_fail_action_cancel_total (no new query key).
# AC4 - no collateral mapping drift: the RetryN on_join_fail=RestartN row
#       (#3052 AC4) is intact; CollectAll writes no on_join_fail; the
#       #4114 FailFast production arm stays gated on
#       production_defaults_active(); the #4114 AC4 fixture pins the
#       superseded CircuitBreaker Cancel face; Soft / Off (production
#       off) keeps ReportOnly.
# AC5 - runtime ACs live in tests/orch/test_failure_policy_bridge.cpp
#       (per #81934 extend-the-family; dispatcher wired), no
#       tests/orch/test_issue_4236.cpp, no docs/design/4236-* (per
#       #1655), no new query key (query:orch-module-stats unchanged, no
#       query:4236), no process-global AgentRegistry; linter registered
#       in build.py and on the root check allowlist.
#
# Self-test:
#   python3 scripts/check_join_fail_circuitbreaker_4236.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    spawn_raw = _read("src/orch/agent_spawn.h")
    spawn = _strip_cpp_comments(spawn_raw)
    scope_raw = _read("src/orch/agent_scope.h")
    scope = _strip_cpp_comments(scope_raw)
    test_src = _read("tests/orch/test_failure_policy_bridge.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")
    obs_src = _read("src/compiler/evaluator_primitives_observability.cpp")

    # -- AC1: the bridge arms the join face under production --
    if "kCircuitBreakerJoinFailProductionArmIssue = 4236" not in spawn:
        fails.append("AC1: #4236 stamp constant missing from agent_spawn.h")
    if "Issue #4236" not in spawn_raw:
        fails.append("AC1: bridge must cite Issue #4236")
    if "production (#4236) also arms on_join_fail=Cancel" not in spawn_raw:
        fails.append("AC1: mapping-table comment must document the #4236 production arm")
    circuit_case = spawn.find("case FP::CircuitBreaker:")
    if circuit_case < 0:
        fails.append("AC1: CircuitBreaker case missing from the bridge switch")
    else:
        window = spawn[circuit_case : spawn.find("}", circuit_case)]
        if "out.on_stall = AgentFailureAction::Cancel;" not in window:
            fails.append("AC1: CircuitBreaker on_stall=Cancel arm missing")
        if "out.consecutive_stall_limit = consecutive_stall_limit;" not in window:
            fails.append("AC1: consecutive_stall_limit threading missing")
        if "if (production_defaults_active())" not in window:
            fails.append("AC1: on_join_fail arm must be gated on production_defaults_active()")
        if "out.on_join_fail = AgentFailureAction::Cancel;" not in window:
            fails.append("AC1: CircuitBreaker must arm on_join_fail=Cancel (production)")

    # -- AC2: explicit post-compose ReportOnly still honored --
    if "w.agent_policy.on_join_fail != AgentFailureAction::ReportOnly" not in scope:
        fails.append("AC2: apply_workflow #3969 route gate missing (explicit override wins)")
    if "ac4236_3_explicit_report_only_observe_first" not in test_src:
        fails.append("AC2: explicit-ReportOnly runtime AC missing from the bridge test")
    if "3969 AC1: ReportOnly FailFast does not request_cancel" not in test_src:
        fails.append("AC2: #3969 AC1 explicit-ReportOnly fixture must stay pinned")

    # -- AC3: production compose routes through compose_supervised_batch --
    if "compose_supervised_batch(sched, scope, tasks, w, stall_timeout_ms, watch_scope)" not in scope:
        fails.append("AC3: apply_workflow must route to compose_supervised_batch")
    if "ac4236_2_circuit_breaker_residual_cancels_scope" not in test_src:
        fails.append("AC3: circuit-open residual runtime AC missing from the bridge test")
    if "4236 AC2: armed Cancel reaches the live Scope agent" not in test_src:
        fails.append("AC3: supervised-path runtime oracle (#4236 AC2) must stay pinned")
    if "agent_join_fail_action_cancel_total" not in scope:
        fails.append("AC3: cancel counter must be reused (no new query key)")

    # -- AC4: no collateral mapping drift --
    if "out.on_join_fail = AgentFailureAction::RestartN;" not in spawn:
        fails.append("AC4: RetryN on_join_fail=RestartN row missing (#3052 AC4 contract)")
    collect_case = spawn.find("case FP::CollectAll:")
    retry_case = spawn.find("case FP::RetryN:")
    if collect_case < 0 or retry_case < 0:
        fails.append("AC4: CollectAll / RetryN cases missing from the bridge")
    else:
        collect_window = spawn[collect_case:retry_case]
        if "on_join_fail" in collect_window:
            fails.append("AC4: CollectAll must not write on_join_fail (unchanged mapping)")
    ff_case = spawn.find("case FP::FailFast:")
    if ff_case < 0:
        fails.append("AC4: FailFast case missing from the bridge switch")
    else:
        ff_window = spawn[ff_case : spawn.find("case FP::CollectAll:", ff_case)]
        if "out.on_join_fail = AgentFailureAction::Cancel;" not in ff_window:
            fails.append("AC4: #4114 FailFast production arm must stay intact")
        if "if (production_defaults_active())" not in ff_window:
            fails.append("AC4: #4114 FailFast arm must stay production-gated")
    if "4114 AC4: CircuitBreaker on_join_fail armed Cancel under production (#4236 supersedes)" not in test_src:
        fails.append("AC4: #4114 AC4 fixture must pin the superseded Cancel face")
    if "4236 AC1: Soft / Off keeps on_join_fail ReportOnly" not in test_src:
        fails.append("AC4: Soft / Off face must stay pinned in the bridge test")

    # -- AC5: runtime ACs hosted + no invent + registration --
    if "ac4236_run_added_tests();" not in test_src:
        fails.append("AC5: bridge-test dispatcher must call ac4236_run_added_tests")
    if (ROOT / "tests/orch/test_issue_4236.cpp").exists():
        fails.append("AC5: tests/orch/test_issue_4236.cpp must not exist (per #81934)")
    if list((ROOT / "docs" / "design").glob("4236-*")):
        fails.append("AC5: docs/design/4236-* must not exist (per #1655)")
    if "query:4236" in spawn or "query:4236" in scope:
        fails.append("AC5: no new query key (query:4236 must not exist)")
    if "query:orch-module-stats" not in obs_src:
        fails.append("AC5: query:orch-module-stats key must stay (no rename)")
    if "class AgentRegistry" in scope:
        fails.append("AC5: no process-global AgentRegistry")
    if "check_join_fail_circuitbreaker_4236" not in build_src:
        fails.append("AC5: linter must be registered in build.py")
    if "check_join_fail_circuitbreaker_4236.py" not in allow_src:
        fails.append("AC5: linter must be on the root check allowlist")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("PASS check_join_fail_circuitbreaker_4236: all ACs green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
