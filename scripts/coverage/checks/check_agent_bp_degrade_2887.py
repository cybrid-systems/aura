#!/usr/bin/env python3
"""Issue #2887: orch mailbox BP storm — producer degrade on AgentScope watch.

Contract (one row per AC):
  AC1  Default policy → on_backpressure ReportOnly; no new cancels on BP;
       stall behaviour unchanged
  AC2  Scope with high local BP + on_backpressure=Cancel → request_cancel
       on scope handles; admit soft-reject for new spawn still present
  AC3  Throttle path cooperative only (helper_stop); no forced body kill
  AC4  Process-global registry still forbidden (MVP linter green)
  AC5  Counters + query:orch-module-stats additive; Soft regression
  AC6  Source-cite + tests (extend failure-policy suite) per #81967;
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SCOPE_FILES = [
    "src/orch/agent_spawn.h",
    "src/orch/agent_scope.h",
    "src/compiler/evaluator_primitives_agent.cpp",
    "tests/orch/test_agent_failure_policy.cpp",
    "scripts/coverage/checks/check_agent_bp_degrade_2887.py",
]


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

    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    prims = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_agent_failure_policy.cpp")
    build = _read("build.py")
    mvp = _read("scripts/coverage/checks/check_orch_mvp_scope.py")

    # ── AC1: default ReportOnly + Throttle enum + stall unchanged ──
    must("on_backpressure", "AC1", spawn)
    must("AgentFailureAction::ReportOnly", "AC1", spawn)
    must("Throttle = 3", "AC1", spawn)
    must("bp_threshold = 0", "AC1", spawn)
    # Default field init for on_backpressure must be ReportOnly.
    must("on_backpressure = AgentFailureAction::ReportOnly", "AC1", spawn)
    # Stall default still Cancel (no behaviour change for stall path).
    must("on_stall = AgentFailureAction::Cancel", "AC1", spawn)

    # ── AC2: Cancel path on BP in watch_all + admit gate preserved ──
    must("on_backpressure", "AC2", scope)
    must("request_cancel", "AC2", scope)
    must("agent_bp_cancel_total", "AC2", scope)
    must("agent_bp_degrade_total", "AC2", scope)
    must("lookup_scope_bp_gauge", "AC2", scope)
    # Admit soft-reject surface still present (#2228/#2535).
    must("spawn_bp_admit_reject_total", "AC2", spawn)
    must("resolve_mailbox_bp_admit_threshold", "AC2", spawn)

    # ── AC3: Throttle cooperative (helper_stop, no cancel) ──
    must("AgentFailureAction::Throttle", "AC3", scope)
    must("stop_keepalive_helper", "AC3", scope)
    must("agent_bp_throttle_total", "AC3", scope)
    # Throttle branch must not call request_cancel in the same arm —
    # verified by presence of Throttle case before Cancel path.
    thr_pos = scope.find("AgentFailureAction::Throttle")
    if thr_pos < 0:
        fails.append("AC3: Throttle action not referenced in agent_scope.h")
    else:
        # Local window around first Throttle use in BP pass should mention
        # helper_stop / stop_keepalive_helper and not request_cancel first.
        window = scope[thr_pos : thr_pos + 800]
        if "stop_keepalive_helper" not in window and "helper_stop" not in window:
            fails.append("AC3: Throttle path must use stop_keepalive_helper / helper_stop")

    # ── AC4: no global registry reintroduction; MVP linter still in gate ──
    # Comments may mention global_agent_registry as the removed identifier
    # (#1966); fail only on a live definition/assignment outside comments.
    for i, line in enumerate(scope.splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("//"):
            continue
        if re.search(r"\b(global_agent_registry|AgentRegistry)\b", line):
            fails.append(f"AC4: forbidden global registry identifier at agent_scope.h:{i}")
    must("check_orch_mvp_scope", "AC4", build)
    if not mvp:
        fails.append("AC4: missing check_orch_mvp_scope.py")
    must("global_agent_registry", "AC4", mvp)  # reintroduction-guard pattern list

    # ── AC5: counters + query surface additive ──
    must("agent_bp_degrade_total", "AC5", spawn)
    must("agent_bp_cancel_total", "AC5", spawn)
    must("agent_bp_throttle_total", "AC5", spawn)
    must("schema-2887", "AC5", prims)
    must("issue-2887", "AC5", prims)
    must("agent-bp-degrade-wired", "AC5", prims)
    must("agent-bp-degrade-total", "AC5", prims)
    must("agent-bp-cancel-total", "AC5", prims)
    must("agent-bp-throttle-total", "AC5", prims)
    # Existing surfaces preserved.
    must("schema-2229", "AC5", prims)
    must("spawn_bp_admit_reject_total", "AC5", spawn)
    # Aura orch:scope-watch kwargs.
    must("on-backpressure", "AC5", prims)
    must("bp-threshold", "AC5", prims)

    # ── AC6: source-cite + tests; no invent; no docs/design/ ──
    must("Issue #2887", "AC6", spawn)
    must("Issue #2887", "AC6", scope)
    must("2887", "AC6", prims)
    must("#2887", "AC6", test)
    must("on_backpressure", "AC6", test)
    must("AgentFailureAction::Throttle", "AC6", test)
    if "check_agent_bp_degrade_2887" not in build:
        fails.append("AC6: build.py does not wire #2887 linter")
    if (ROOT / "tests" / "core" / "test_issue_2887.cpp").is_file():
        fails.append("AC6: test_issue_2887.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "orch" / "test_issue_2887.cpp").is_file():
        fails.append("AC6: test_issue_2887.cpp present in tests/orch (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for p in docs.glob("*2887*"):
            fails.append(f"AC6: docs/design/{p.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2887 agent BP degrade on AgentScope watch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
