#!/usr/bin/env python3
"""Issue #4050: supervise-batch watch path never joined the child scope and
RestartN stalled on an unstamped coop clock.

Contract (one row per AC):
  AC1  orch:supervise-batch :watch-scope #t closes the child scope before
       returning (production only) with the existing budgets — the
       ~AgentScope wait (kDefaultJoinDrainMs) plus the residual drain SSOT
       (kResidualJoinDrainMs) whose cleanup goes through
       complete_agent_join_cleanup / ensure_reclaimed_cleanup. The join
       policy is an explicit ReportOnly policy so the close cannot cancel,
       restart or replace a live body. Soft adds no wait (#2585 / #2661).
  AC2  the supervise spec is a one-shot fiber-clock body
       (AgentSpec::body_clock_from_fiber): the closure wrapper stamps
       progress at apply_closure return and watch_agent_liveness treats a
       finished fiber as Done and never issues the stall cancel for it
       (no agent_poll contract => no kill), so a closure longer than the
       #2585 coop window is not cancelled merely for a missing agent_poll.
  AC3  RestartN + keepalive_interval_ms == 0 does NOT reuse the
       restart_skipped_no_spec arm when a restartable spec exists: the
       Closed arm surfaces scope_stalled instead, and the watch RestartN
       arm defers (restart_deferred_body_live, no cancel/drain/replace) for
       the one-shot slot. #3250 / #3730 spec-missing row is preserved.
  AC4  the supervise hash carries the additive supervision keys
       (scope-stalled / restart-ok / restart-denied /
       restart-skipped-no-spec / restart-deferred-body-live) and reports
       ok=false when production RestartN cancelled, skipped or deferred a
       still-live body (or left a slot stalled).
  AC5  one SSOT projection (project_scope_watch_) feeds
       ApplyWorkflowResult for compose_supervised_batch / apply_workflow /
       run_workflow so the hash cannot drift from ScopeWatchResult.
  AC6  ACs live in tests/orch/test_failure_policy_bridge.cpp (the #3495 /
       #3726 host dispatched by tests/orch/test_orch_agent_batch.cpp); no
       tests/**/test_issue_4050.cpp; no docs/design/4050-*
  AC7  no AgentRegistry, no new query key, no rename of
       query:orch-module-stats; additive issue stamp only.
  AC8  build.py wires check_supervise_join_4050 + root allowlist row.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    test = _read("tests/orch/test_failure_policy_bridge.cpp")
    dispatcher = _read("tests/orch/test_orch_agent_batch.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: watch-scope child close on the existing budgets ─────────────
    must("Issue #4050", "AC1 prim cites", prim)
    must("kSuperviseBatchScopeJoinIssue = 4050", "AC1 issue stamp", spawn)
    must(
        "if (watch_scope && aura::compiler::typed_audit::production_defaults_active()) {",
        "AC1 production-gated close",
        prim,
    )
    must("child.join_all(join_policy, join_observe)", "AC1 child join", prim)
    must("join_policy.primary_ms = aura::orch::kDefaultJoinDrainMs;", "AC1 wait budget SSOT", prim)
    must("join_policy.drain_ms = aura::orch::kResidualJoinDrainMs;", "AC1 drain budget SSOT", prim)
    must(
        "join_observe.on_join_fail = aura::orch::AgentFailureAction::ReportOnly;",
        "AC1 close cannot act on join fail",
        prim,
    )
    must("join_observe.on_stall = aura::orch::AgentFailureAction::ReportOnly;", "AC1 close cannot stall-act", prim)
    must("Soft keeps the historical no-extra-wait behaviour (#2585 / #2661)", "AC1 Soft no wait", prim)

    # ── AC2: one-shot fiber-clock body, no cancel for missing agent_poll ─
    must("body_clock_from_fiber = true;", "AC2 supervise spec flag", prim)
    must("(void)aura::orch::agent_poll();", "AC2 progress at apply_closure return", prim)
    must("bool body_clock_from_fiber = false;", "AC2 AgentSpec flag", spawn)
    must("bool body_clock_from_fiber = false) {", "AC2 watch param", spawn)
    must("if (body_clock_from_fiber && h.fiber && h.fiber->is_done()) {", "AC2 finished closure Done", spawn)
    must("if (cancel_on_stall && !body_clock_from_fiber) {", "AC2 no kill without a poll contract", spawn)

    # ── AC3: RestartN + keepalive==0 boundary ────────────────────────────
    must("if (body_clock_from_fiber && !restart_spec_missing_(i)) {", "AC3 spec-aware Closed arm", scope)
    must(
        "++r.restart_skipped_no_spec;\n                            note_restart_skipped_no_spec_(h, /*cancel=*/false);",
        "AC3 #3730 spec-missing row preserved",
        scope,
    )
    must("if (i < specs_.size() && specs_[i].body_clock_from_fiber) {", "AC3 restart defer arm", scope)
    must("++r.restart_deferred_body_live;", "AC3 deferred replace reported", scope)
    must(
        "const bool cancel_on_stall = (policy.on_stall != AgentFailureAction::ReportOnly);",
        "AC3 stall arm unchanged",
        scope,
    )
    # Format-robust: clang-format reflows the initializer / call.
    must("specs_[i].body_clock_from_fiber", "AC3 watch_all reads the spec flag", scope)
    must(
        "watch_agent_liveness(h, stall_timeout_ms, cancel_on_stall, body_clock_from_fiber)",
        "AC3 watch passes the spec flag",
        scope,
    )

    # ── AC4: additive supervision keys + ok=false when dirty ─────────────
    for key, label in (
        ('{"scope-stalled"', "AC4 scope-stalled"),
        ('{"restart-ok"', "AC4 restart-ok"),
        ('{"restart-denied"', "AC4 restart-denied"),
        ('{"restart-skipped-no-spec"', "AC4 restart-skipped-no-spec"),
        ('{"restart-deferred-body-live"', "AC4 restart-deferred-body-live"),
        ('{"schema-4050"', "AC4 schema-4050"),
    ):
        must(key, label, prim)
    must("const bool supervise_ok =", "AC4 ok computed from the outcome", prim)
    must("(r.batch.status == BatchStatus::Ok) &&", "AC4 batch status still required", prim)
    must(
        "aura::compiler::typed_audit::production_defaults_active() && restart_dirty",
        "AC4 production-only ok downgrade",
        prim,
    )
    must("r.restart_deferred_body_live > 0 ||", "AC4 deferred counts as dirty", prim)
    must("r.scope_stalled > 0;", "AC4 stalled counts as dirty", prim)

    # ── AC5: single projection SSOT ──────────────────────────────────────
    must("inline void project_scope_watch_(ApplyWorkflowResult& out,", "AC5 projection SSOT", scope)
    must(
        "out.restart_deferred_body_live = static_cast<int>(wres.restart_deferred_body_live);",
        "AC5 deferred projected",
        scope,
    )
    must("project_scope_watch_(out, wres);", "AC5 apply_workflow projects", scope)
    must("project_scope_watch_(stage, wres);", "AC5 run_workflow projects", scope)
    must("int restart_deferred_body_live = 0;", "AC5 result field", spawn)

    # ── AC6: test host + no new files ────────────────────────────────────
    must("Issue #4050", "AC6 test cites", test)
    must("#4050 AC1", "AC6 test AC1", test)
    must("#4050 AC4", "AC6 test AC4", test)
    must("ac4050_2_long_closure_defers_not_cancels", "AC6 soak AC", test)
    must("ac4050_run_added_tests", "AC6 runner", test)
    must("run_test_failure_policy_bridge", "AC6 dispatcher entry", dispatcher)
    if _read("tests/orch/test_issue_4050.cpp"):
        fails.append("AC6: tests/orch/test_issue_4050.cpp must not exist")
    if _read("tests/issues/test_issue_4050.cpp"):
        fails.append("AC6: tests/issues/test_issue_4050.cpp must not exist")
    if _read("docs/design/4050-supervise-join.md"):
        fails.append("AC6: docs/design/4050-* must not exist")

    # ── AC7: no registry / no new query key ──────────────────────────────
    forbid("class AgentRegistry", "AC7 no AgentRegistry", spawn)
    forbid("query:4050", "AC7 no query:4050", prim)
    forbid("query:supervise-batch", "AC7 no new query key", prim)
    must("query:orch-module-stats", "AC7 existing stats query key untouched", prim)

    # ── AC8: build.py wiring + root allowlist ────────────────────────────
    must("check_supervise_join_4050", "AC8 build.py wires linter", build)
    must("check_supervise_join_4050.py", "AC8 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"4050 FAIL: {f}", file=sys.stderr)
        return 1
    print("4050 OK: supervise-batch child join / one-shot body clock contracts satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
