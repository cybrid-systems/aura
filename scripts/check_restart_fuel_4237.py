#!/usr/bin/env python3
"""Issue #4237: RetryN compose arms RestartN with max_restarts=0 /
keepalive=0 — restart belief without re-spawn (P2 orch closed-loop).

Hole: to_agent_policy(RetryN) / compose_workflow_policy(RetryN) arm
on_stall=RestartN AND on_join_fail=RestartN (#3052 AC4), so the composed
WorkflowFailurePolicy reads as a commercial restart strategy. Two fuel
gates then no-op the action:

  1. Default max_restarts=0 when compose omits max_retries →
     apply_on_join_fail_unlocked_ evaluates restart_counts_[i] < 0-false →
     agent_restart_exhausted_total only (no re-spawn).
  2. watch_all RestartN requires keepalive / a restartable spec:
     keepalive_interval_ms==0 → Closed / restart_skipped_no_spec (#3730);
     bare adopt without spec → skip+Cancel under production (#3250).

Closed-loop belief: hash / policy say "restart-n" while a long-run
multi-agent Scope never restarts one-shot / zero-fuel agents.

Fix shape (one closed-loop observe face; no second orch model; no deny):
  - compose derives restart_fuel_missing once (pure predicate
    agent_restart_fuel_missing): a RestartN arm (on_stall and/or
    on_join_fail) with max_restarts == 0 is belief without fuel.
  - The Aura compose hash surfaces "restart-fuel-missing" next to the
    already-projected "max-restarts" int so hosts / agents cannot treat
    "restart-n" as live fuel when 0.
  - Runtime no-op faces stay verbatim: agent_restart_exhausted_total
    (join arm) and agent_restart_skipped_no_spec_total (#3730/#3250
    watch + join skip) — no new counter, no new query key.
  - Soft / Off: policy behaviour unchanged (observe-only in both faces);
    fuel-present RetryN (max_retries>0) composes exactly as before.

Contract (one row per AC):
  AC1  agent_spawn.h: compose derives the fuel face in BOTH overloads via
       the pure predicate; issue stamp kRestartFuelMissingIssue = 4237;
       RetryN mapping-table doc names the 0-fuel observe arm
  AC2  evaluator_primitives_agent.cpp: compose hash surfaces
       "restart-fuel-missing"; no new query key (query:4237 absent)
  AC3  agent_scope.h: exhausted + skip counters still the runtime no-op
       faces; no new compose counter invented
  AC4  runtime ACs dispatched in test_failure_policy_bridge.cpp
       (ac4237_1..5); no tests/**/test_issue_4237.cpp;
       no docs/design/4237-*
  AC5  build.py wiring + root_check_allowlist.txt entry

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

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    scope_hdr = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    bridge = _read("tests/orch/test_failure_policy_bridge.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: compose derives the fuel face in BOTH overloads ────────────
    must("kRestartFuelMissingIssue = 4237", "AC1 issue stamp", spawn)
    must("bool restart_fuel_missing = false;", "AC1 struct field appended", spawn)
    must(
        "[[nodiscard]] inline bool agent_restart_fuel_missing(const AgentFailurePolicy& ap) noexcept {",
        "AC1 pure predicate defined",
        spawn,
    )
    must(
        "ap.on_stall == AgentFailureAction::RestartN ||",
        "AC1 predicate covers the on_stall arm",
        spawn,
    )
    must(
        "ap.on_join_fail == AgentFailureAction::RestartN;",
        "AC1 predicate covers the on_join_fail arm",
        spawn,
    )
    hits = spawn.count("w.restart_fuel_missing = agent_restart_fuel_missing(w.agent_policy);")
    if hits < 2:
        fails.append(f"AC1 compose overloads deriving the face: expected >=2, found {hits}")
    must("#4237: max_restarts=0 (the default) is restart", "AC1 mapping-table doc", spawn)

    # ── AC2: Aura compose hash surfaces the face; no new query key ──────
    must('"restart-fuel-missing", make_bool(w.restart_fuel_missing)', "AC2 hash kv", prim)
    must('"restart-backoff-ms"', "AC2 neighbouring max-restarts projection kept", prim)
    absent("query:4237", "AC2 no new query key", prim)
    absent("restart_fuel_missing_total", "AC2 no new compose counter", prim)

    # ── AC3: runtime no-op faces verbatim (no second model) ─────────────
    must("agent_restart_exhausted_total", "AC3 join-arm exhausted counter kept", scope_hdr)
    must("agent_restart_skipped_no_spec_total", "AC3 watch/join skip counter kept", scope_hdr)
    must("restart_spec_missing_", "AC3 #3250 spec-fuel predicate kept", scope_hdr)
    must("keepalive_interval_ms==0", "AC3 #3730 keepalive face referenced", scope_hdr)
    absent("restart_fuel_missing", "AC3 scope header untouched (no second model)", scope_hdr)

    # ── AC4: runtime ACs dispatched; no invented test / docs ────────────
    for fn in (
        "ac4237_1_compose_retryn_default_fuel_missing",
        "ac4237_2_retryn_with_fuel_not_missing",
        "ac4237_3_soft_face_unchanged_no_deny",
        "ac4237_4_other_policies_never_fuel_missing",
        "ac4237_5_source_cite_no_invent",
    ):
        must(fn, "AC4 bridge AC defined", bridge)
        must(fn + "();", "AC4 bridge AC dispatched", bridge)
    must("ac4237_run_added_tests();", "AC4 runner wired into run_test", bridge)
    if (ROOT / "tests" / "orch" / "test_issue_4237.cpp").exists():
        fails.append("AC4: tests/orch/test_issue_4237.cpp must not exist (#81934)")
    if (ROOT / "tests" / "core" / "test_issue_4237.cpp").exists():
        fails.append("AC4: tests/core/test_issue_4237.cpp must not exist (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir() and any(design_dir.glob("4237-*")):
        fails.append("AC4: no docs/design/4237-*")

    # ── AC5: build.py + allowlist wiring ────────────────────────────────
    must("check_restart_fuel_4237.py", "AC5 build.py wires the linter", build)
    must("check_restart_fuel_4237.py", "AC5 root allowlist entry", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("check_restart_fuel_4237: OK (5 AC rows satisfied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
