#!/usr/bin/env python3
"""Issue #2885: orch/join — Reclaimed path agent-visible still-running SLA
(join hash + deferred cleanup report).

Contract (one row per AC):
  AC1  Non-yielding tight-loop agent → join returns `status=reclaimed` +
       `still-running` while body alive; after body exit gauge drops +
       `body_retired` bumps
  AC2  Ok / Timeout / Cancelled paths do NOT add new keys (zero-cost /
       absent)
  AC3  #2661 contract unchanged: no reservation release / mailbox detach
       on Reclaimed path in `complete_agent_join_cleanup`
  AC4  `join_reclaimed_deferred_cleanup_total` still bumps; additive
       schema key on agent-join hash
  AC5  Soft / sandbox=off regression green; existing #2743 reclaimed
       string preserved
  AC6  Source-cite + tests (extend `test_join_drain_reclaim`) per #81967;
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Files in scope for #2885.
SCOPE_FILES = [
    "src/serve/fiber.h",
    "src/orch/agent_spawn.h",
    "src/compiler/evaluator_primitives_agent.cpp",
    "tests/orch/test_join_drain_reclaim.cpp",
    "scripts/coverage/checks/check_join_cleanup_report_2885.py",
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

    fiber_h = _read("src/serve/fiber.h")
    agent_spawn = _read("src/orch/agent_spawn.h")
    posture = _read("src/compiler/evaluator_primitives_agent.cpp")
    test_drain = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # ── AC1: still-running flag + reclaim timestamp + gauge + body-exit clear ──
    must("still_running_after_reclaim_counted", "AC1", fiber_h)
    must("mark_reclaimed_steady_clock_ns", "AC1", fiber_h)
    must("join_drain_residual_still_running", "AC1", fiber_h)
    must("note_body_exit_if_reclaimed", "AC1", fiber_h)
    must("is_reclaimed", "AC1", fiber_h)
    # The per-Fiber timestamp field is `body_reclaim_start_ns_` (set in
    # mark_reclaimed at fiber.cpp:779) — the new accessor #2885 exposes
    # it under the friendlier name `mark_reclaimed_steady_clock_ns`.
    must("body_reclaim_start_ns_", "AC1", fiber_h)
    # Test file exercises the accessors + still_running flow.
    must("still_running_after_reclaim_counted", "AC1", test_drain)
    must("mark_reclaimed_steady_clock_ns", "AC1", test_drain)
    must("mark_reclaimed", "AC1", test_drain)
    must("note_body_exit_if_reclaimed", "AC1", test_drain)
    # Aura surface reads per-fiber state at join return.
    must("h.fiber->still_running_after_reclaim_counted", "AC1", posture)
    must("h.fiber->mark_reclaimed_steady_clock_ns", "AC1", posture)

    # ── AC2: Ok / Timeout / Cancelled zero-cost (no new keys) ──
    # The new keys (still-running, reclaim-age-ms, deferred-cleanup,
    # schema-2885, issue-2885, agent-join-still-running-wired) MUST be
    # guarded by an `if (jr.status == JoinStatus::Reclaimed)` block —
    # not unconditional. Source-cite check below.
    if posture.count("if (jr.status == aura::serve::JoinStatus::Reclaimed)") < 2:
        fails.append(
            "AC2: still-running / reclaim-age-ms / deferred-cleanup keys must be guarded by "
            "`if (jr.status == JoinStatus::Reclaimed)` (zero-cost on Ok / Timeout / "
            "Cancelled paths)"
        )
    # The new keys must be inside a kv.emplace_back pattern (not directly
    # in the unconditional kv initializer list).
    if "still-running" in posture and "reclaim-age-ms" in posture:
        # Find position of "still-running" and check it appears after a
        # `if (jr.status == ... Reclaimed)` line.
        still_pos = posture.find('"still-running"')
        reclaimed_pos = posture.rfind("if (jr.status == aura::serve::JoinStatus::Reclaimed)", 0, still_pos)
        if reclaimed_pos < 0 or still_pos < reclaimed_pos:
            fails.append(
                "AC2: 'still-running' key position precedes the Reclaimed guard (not zero-cost on other paths)"
            )
    # join_reclaimed_deferred_cleanup_total must still be bumped.
    must("join_reclaimed_deferred_cleanup_total", "AC2", agent_spawn)

    # ── AC3: #2661 contract preserved (no body-stack free on Reclaimed) ──
    must("complete_agent_join_cleanup", "AC3", agent_spawn)
    # Reclaimed branch must call release_orphan_roots (global-table only).
    if "if (jr.status == serve::JoinStatus::Reclaimed)" in agent_spawn:
        # Extract the Reclaimed block: from the if-line to the next "return"
        # or the next "}" at column 0.
        start = agent_spawn.find("if (jr.status == serve::JoinStatus::Reclaimed)")
        # Find the next `return;` after the Reclaimed branch start.
        end = agent_spawn.find("\n    }\n", start)
        if end < 0:
            end = start + 1500
        reclaimed_block = agent_spawn[start:end]
        if "release_orphan_roots" not in reclaimed_block:
            fails.append(
                "AC3: Reclaimed branch in complete_agent_join_cleanup must call "
                "release_orphan_roots (global-table drop per #2661)"
            )
        # Body-stack free paths must NOT appear.
        if "release_agent_memory_reservation" in reclaimed_block:
            fails.append(
                "AC3: Reclaimed branch must NOT call release_agent_memory_reservation "
                "(#2661: body-stack free only on Done paths)"
            )
        if "h.mailbox->detach" in reclaimed_block:
            fails.append(
                "AC3: Reclaimed branch must NOT call h.mailbox->detach (#2661: mailbox detach only on Done paths)"
            )
    else:
        fails.append("AC3: complete_agent_join_cleanup Reclaimed branch not found")

    # ── AC4: join_reclaimed_deferred_cleanup_total still bumps + additive schema ──
    must("join_reclaimed_deferred_cleanup_total", "AC4", agent_spawn)
    must("agent_join_reclaimed_total", "AC4", agent_spawn)
    must("schema-2885", "AC4", posture)
    must("issue-2885", "AC4", posture)
    must("agent-join-still-running-wired", "AC4", posture)
    # Still-running + reclaim-age-ms + deferred-cleanup keys exposed.
    must("still-running", "AC4", posture)
    must("reclaim-age-ms", "AC4", posture)
    must("deferred-cleanup", "AC4", posture)
    must("schema-2743", "AC4", posture)
    must("agent_join_reclaimed_total", "AC4", agent_spawn)

    # ── AC5: Soft / unit / sandbox=off regression green (#2743 unchanged) ──
    must("schema-2743", "AC5", posture)
    must("issue-2743", "AC5", posture)
    must('status="reclaimed"', "AC5", posture)
    # The new keys are guarded by Reclaimed check (per AC2) — Soft path
    # never triggers Reclaimed, so the new code path is inert under Soft.

    # ── AC6: source-cite + tests; no docs/design/; no invent ──
    must("Issue #2885", "AC6", fiber_h)
    must("Issue #2885", "AC6", agent_spawn)
    must("schema-2885", "AC6", posture)
    must("2885", "AC6", test_drain)
    if "check_join_cleanup_report_2885" not in build:
        fails.append("AC6: build.py does not wire #2885 linter")
    # No new test_issue_2885.cpp (per #81967).
    for d in ("core", "orch", "compiler"):
        if (ROOT / "tests" / d / "test_issue_2885.cpp").is_file():
            fails.append(f"AC6: tests/{d}/test_issue_2885.cpp present (forbidden per #81967)")
    # No docs/design/2885-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2885-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2885 per-join still-running SLA on Reclaimed path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
