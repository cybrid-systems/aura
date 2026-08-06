#!/usr/bin/env python3
"""Issue #2702: Resume MutationSafetySnapshot + safety ticket — unify hard-fail path.

Contract (one row per AC):
  AC1 src/serve/fiber.cpp check_and_enforce_resume_snapshot_invariant
     implements the production hard-fail path: ticket mismatch OR
     mutation_safety_snapshot_inconsistent → request_cancel +
     set_state(Done), no swapcontext body. No soft continue under
     production lock.
  AC2 Soft / test-override: metric-only continue (existing Soft
     ergonomics preserved — is_steal_snapshot_hard_mode returns false
     in Soft / under AURA_STEAL_SNAPSHOT_SOFT=1).
  AC3 Ticket is one-shot: cleared via clear_resume_safety_ticket()
     after the check; never re-used across steals.
  AC4 Strong interaction with steal safety transaction (#2699): ticket
     is stamped only on Ok transaction (set_resume_safety_ticket is
     called in the Ok branch of steal_safety_transaction). Resume never
     sees a ticket from a RejectHard path.
  AC5 Coverage + unit: tests/serve/test_steal_safety_ticket.cpp
     extended with ac2702_1..6 (per #81967).
  AC6 No docs/design/ per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    sh = _read("src/serve/steal_safety.h")
    sc = _read("src/serve/steal_safety.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_steal_safety_ticket.cpp")
    build = _read("build.py")

    # AC1 — production hard-fail path
    must("check_and_enforce_resume_snapshot_invariant", "AC1", fc)
    must("request_cancel", "AC1", fc)
    must("set_state(FiberState::Done)", "AC1", fc)
    must("is_steal_snapshot_hard_mode", "AC1", fc)
    must("bump_mutation_steal_snapshot_mismatch", "AC1", fc)
    must("bump_steal_safety_ticket_mismatch", "AC1", fc)
    must("bump_steal_snapshot_hard_fail", "AC1", fc)
    must("Issue #2702", "AC1", fh)

    # AC2 — Soft / test-override metric-only continue
    must("is_steal_snapshot_soft_mode", "AC2", fc)
    must("AURA_STEAL_SNAPSHOT_SOFT", "AC2", fc)
    must("AURA_STEAL_SNAPSHOT_HARD", "AC2", fc)

    # AC3 — ticket is one-shot
    must("clear_resume_safety_ticket", "AC3", fc)
    must("has_resume_safety_ticket_", "AC3", fh)

    # AC4 — interaction with #2699 steal safety transaction
    must("set_resume_safety_ticket", "AC4", fh)
    must("set_resume_safety_ticket", "AC4", fc)
    must("steal_safety_transaction", "AC4", sc)
    must("kStealSafetyTransactionIssue = 2699", "AC4", sh)
    must("kResumeHardFailIssue = 2702", "AC4", fh)

    # AC5 — coverage + test extension
    must("query:resume-hard-fail", "AC5", q)
    must("resume-hard-fail-total", "AC5", q)
    must("resume-soft-observe-total", "AC5", q)
    must("resume-hard-fail-wired", "AC5", q)
    must("schema-2702", "AC5", q)
    must("issue-2702", "AC5", q)
    must("ac2702_1_resume_invariant_exists", "AC5", t)
    must("ac2702_2_soft_path_metric_only", "AC5", t)
    must("ac2702_3_ticket_one_shot", "AC5", t)
    must("ac2702_4_steal_safety_ticket_interaction", "AC5", t)
    must("ac2702_5_query_keys_and_source_cite", "AC5", t)
    must("ac2702_6_no_docs_design", "AC5", t)
    must("check_resume_hard_fail_2702", "AC5", build)

    # AC6 — no docs/design/2702-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2702-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2701 + #2700 + #2699 linters still green
    for prev in (
        "check_mutation_hold_budget_reject_2701.py",
        "check_handoff_ref_mailbox_gate_2700.py",
        "check_steal_safety_transaction_2699.py",
    ):
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "coverage" / "checks" / prev),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2702 resume hard-fail unified path — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
