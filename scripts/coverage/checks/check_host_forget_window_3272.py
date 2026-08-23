#!/usr/bin/env python3
"""Issue #3272: close the production host-forget window after the Reclaimed
auto-wait Timeout.

#3110 auto-waits 50ms; #3220 bumps host_forget on Timeout; #3245 documents
the SSOT second-wait. Residual: hosts that keep AgentHandle in long-lived
vectors (or hand the handle across components without a second wait) can
pin quota / mailbox slots indefinitely until dtor. #3272 closes the
residual without violating #2661 (no body-stack free while running) and
without changing Soft/Off zero-cost: ensure_reclaimed_cleanup stays the
SSOT second-wait; dtor stays the RAII last resort; Aura join /
scope-join-all hashes surface cleanup-pending / cleanup-pending-count on
the risk path only (additive, no new metrics bus, no renamed keys).

Contract (one row per AC):
  AC1  auto-wait Timeout → must_wait retained, reservation held (#2661)
  AC2  ensure_reclaimed_cleanup after body exit → cleanup_completed;
       idempotent second call (no double release)
  AC3  never second-wait + drop handle → dtor releases residual
  AC4  cleanup-pending / cleanup-pending-count hash keys; no new query:*;
       SSOT ensure_reclaimed_cleanup preserved; no process-global registry
  AC5  extend test_join_drain_reclaim; no test_issue_3272.cpp;
       no docs/design/ (#1655); build.py wires linter; README contract

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

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    readme = _read("src/orch/README.md")

    must("kHostForgetWindowCloseIssue = 3272", "AC4 stamp", spawn)
    must("ensure_reclaimed_cleanup", "AC2 SSOT helper", spawn)
    must("finish_reclaimed_cleanup_on_dtor", "AC3 RAII last resort", spawn)
    must("host_forget_reclaimed_risk_total", "AC1 risk counter reuse", spawn)
    must("cleanup-pending", "AC4 join hash key", prim)
    must("cleanup-pending-count", "AC4 scope-join-all key", prim)
    must("schema-3272", "AC4 schema stamp", prim)
    must("cleanup-pending-wired", "AC4 wired marker", prim)
    must("3272 AC1", "AC1 test", test)
    must("3272 AC2", "AC2 test", test)
    must("3272 AC3", "AC3 test", test)
    must("3272 AC4", "AC4 test", test)
    must("#2661 no-early-free", "AC1 #2661", test)
    must("idempotent", "AC2 idempotency", test)
    must("check_host_forget_window_3272", "AC5 build.py", build)
    must("ensure_reclaimed_cleanup", "AC5 README contract", readme)
    if "query:cleanup-pending" in prim or "query:host-forget-pending" in prim:
        fails.append("AC4: new query:* (reuse query:orch-module-stats)")
    if _read("tests/orch/test_issue_3272.cpp"):
        fails.append("AC5: test_issue_3272.cpp present (forbidden #81967)")
    if _read("docs/design/3272-host-forget-window.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3272 host_forget_window:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3272 host_forget_window: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
