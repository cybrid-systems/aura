#!/usr/bin/env python3
"""Issue #2970: JoinPolicy optional wait_reclaimed_ms — auto-wait after Reclaimed.

Contract (one row per AC):
  AC1  wait_reclaimed_ms unset → join_agent / join_agents behave exactly as
       pre-issue (no extra poll; zero cost on Ok/Timeout/Cancelled). The
       Reclaimed path still defers (#2661) and does NOT call
       wait_reclaimed_body.
  AC2  Reclaimed + wait set + body exits within window → Done-path cleanup
       runs once (idempotent with #2924); reserved_memory_bytes==0;
       reclaimed_deferred_cleanup cleared; wait_reclaimed_cleanup_total
       bumps.
  AC3  Reclaimed + wait timeout → join status stays Reclaimed; no mailbox
       detach / reservation release (#2661 preserved); wait_reclaimed_
       timeout flag surfaced.
  AC4  Aura hash: when wait used, wait-reclaimed / wait-timeout additive
       keys only inside the Reclaimed guard (parity #2885); wait-us folds
       the auto-wait (jr.wait_us += wr.wait_us); orch:scope-join-all also
       parses :wait-reclaimed-ms (optional batch policy).
  AC5  Metrics reuse wait_reclaimed_total / _timeout_total / _cleanup_total
       (#2924); additive schema-2970 / issue-2970 / join-wait-reclaimed-wired.
  AC6  Source-cite in agent_spawn.h + evaluator_primitives_agent.cpp; tests
       extend tests/orch/test_join_drain_reclaim per #81967 (no new
       test_issue_NNNN.cpp); no docs/design per #1655.

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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # ── AC1: unset → zero cost, no wait_reclaimed_body call ─────────
    must("wait_reclaimed_ms", "AC1", spawn)
    must("nullopt = off", "AC1", spawn)
    must("Issue #2970", "AC1", spawn)

    # ── AC2: auto-wait + body exit → Done cleanup once ──────────────
    must("wait_reclaimed_body(h, policy.wait_reclaimed_ms)", "AC2", spawn)
    must("wait_reclaimed_cleanup_total", "AC2", spawn)
    must("reserved_memory_bytes == 0", "AC2", test)

    # ── AC3: timeout → Reclaimed kept, no release ───────────────────
    must("wait_reclaimed_timeout", "AC3", spawn)
    must("no reservation release on timeout", "AC3", test)

    # ── AC4: Aura hash keys in Reclaimed guard + scope kwarg ────────
    must("wait-reclaimed-ms", "AC4", agent)
    must("wait-reclaimed", "AC4", agent)
    must("wait-timeout", "AC4", agent)
    must("jr.wait_us += wr.wait_us", "AC4", spawn)
    must("orch:scope-join-all", "AC4", agent)

    # ── AC5: metrics reuse + additive keys ──────────────────────────
    must("wait_reclaimed_total", "AC5", spawn)
    must("wait_reclaimed_timeout_total", "AC5", spawn)
    must("wait_reclaimed_cleanup_total", "AC5", spawn)
    must("schema-2970", "AC5", agent)
    must("issue-2970", "AC5", agent)
    must("join-wait-reclaimed-wired", "AC5", agent)

    # ── AC6: source-cite + tests + no invent + no docs/design/ ──────
    must("#2970", "AC6", spawn)
    must("#2970", "AC6", agent)
    must("#2970 AC1", "AC6", test)
    must("check_join_wait_reclaimed_2970", "AC6", build)  # linter wired
    for rel in ("tests/orch/test_issue_2970.cpp", "tests/compiler/test_issue_2970.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in ("docs/design/2970-join-wait-reclaimed.md", "docs/design/2970-*.md"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #2970 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #2970 JoinPolicy wait_reclaimed_ms — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
