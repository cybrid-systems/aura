#!/usr/bin/env python3
"""Issue #3111: Fiber steal × held_ref / handoff_completed consistency
residual after agent_send.

Contract (one row per AC):
  AC1  Post-steal re-validate held_ref messages in fiber's mailbox.
       Counter + helper wired into multi_fiber_mailbox.h; call wired into
       aura_evaluator_on_steal_complete strong def in evaluator_fiber_mutation.cpp.
  AC2  Soft / sandbox=off: counter bumps only; may still deliver.
       Production gate via typed_audit::production_defaults_active() preserved.
  AC3  Push-time held_ref gate (#2663 / #3013) unchanged.
  AC4  Additive counters only (held_ref_post_steal_check_total +
       held_ref_stale_after_steal_total). Reuse existing handoff_reject / steal_complete
       counters; no new query key.
  AC5  Extend existing steal × mailbox / handoff suites
       (tests/serve/test_steal_complete_restamp_txn.cpp). No new
       test_issue_3111.cpp. Source-cite + coverage linter wired.
  AC6  No docs/design/3111-* per #1655. Soft/Off zero-cost preserved.
       No Soft treated as vulnerability.

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

    mfbh = _read("src/serve/multi_fiber_mailbox.h")
    ev_fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # ── AC1: post-steal re-validate hook wired (counter + helper + call site) ─
    must("held_ref_post_steal_check_total", "AC1 post-steal check counter in multi_fiber_mailbox.h", mfbh)
    must("held_ref_stale_after_steal_total", "AC1 stale counter in multi_fiber_mailbox.h", mfbh)
    must("revalidate_held_ref_after_steal()", "AC1 revalidate helper in multi_fiber_mailbox.h", mfbh)
    must("bump_held_ref_stale_after_steal()", "AC1 bump helper in multi_fiber_mailbox.h", mfbh)
    must(
        "aura::serve::mf_mailbox::revalidate_held_ref_after_steal",
        "AC1 call site in evaluator_fiber_mutation.cpp (steal-complete strong def)",
        ev_fiber_mut,
    )
    must("aura_evaluator_on_steal_complete", "AC1 strong def context (steal-complete entry point)", ev_fiber_mut)
    must("3111 AC1", "AC1 test marker", test)

    # ── AC2: Soft / sandbox=off counter-only via production gate ────────────
    must(
        "typed_audit::production_defaults_active()", "AC2 production gate preserved (Soft/Off zero-cost)", ev_fiber_mut
    )
    # The call must be wrapped in `if (... && fiber_ptr)` per the strong-def body
    must(
        "if (aura::compiler::typed_audit::production_defaults_active() && fiber_ptr)",
        "AC2 Soft/Off gating present at call site",
        ev_fiber_mut,
    )

    # ── AC3: push-time held_ref gate unchanged (#2663 / #3013) ─────────────
    must("if (msg.held_ref_token.has_value() && !msg.handoff_completed)", "AC3 push-time held_ref gate unchanged", mfbh)
    must("handoff_reject_total", "AC3 push-time handoff_reject counter preserved", mfbh)
    must("3111 AC3", "AC3 test marker", test)

    # ── AC4: additive counters only (no new query key, no middle metrics) ─
    must("held_ref_post_steal_check_total", "AC4 additive check counter", mfbh)
    must("held_ref_stale_after_steal_total", "AC4 additive stale counter", mfbh)
    # Reuse existing steal_complete counter (do NOT add a new steal_complete variant)
    must("steal_complete_total", "AC4 reuse existing steal_complete_total counter", ev_fiber_mut)
    # No new query key added (e.g., no new insert_kv for held_ref_post_steal_*)
    must("3111 AC4", "AC4 test marker", test)

    # ── AC5: extend existing test, no test_issue_NNNN.cpp, linter wired ───
    # Use inline CHECK markers ("3111 AC1" etc) consistent with #3109/#3110 linter
    # pattern (the test embeds these markers in the run_test_ scope, not as
    # separate helper functions — source-cite pattern).
    for ac in ("3111 AC1", "3111 AC2", "3111 AC3", "3111 AC4", "3111 AC5", "3111 AC6"):
        must(ac, f"AC5 test marker {ac}", test)
    # Linter wired in build.py
    must("check_mailbox_held_ref_steal_3111", "AC5 build.py wires 3111 linter", build)
    must("Issue #3111", "AC5 linter error message", build)
    # No invent
    if (ROOT / "tests" / "serve" / "test_issue_3111.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3111.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3111.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3111.cpp present (forbidden #81967)")

    # ── AC6: no docs/design/*, Soft/Off zero-cost preserved ────────────────
    must("3111 AC6", "AC6 test marker", test)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3111-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    # Soft/Off zero-cost: the revalidate call is gated on
    # production_defaults_active() — so Soft / Off / AURA_SANDBOX=off
    # does NOT call revalidate (zero new cost on the quiet path).
    must(
        "if (aura::compiler::typed_audit::production_defaults_active() && fiber_ptr) {",
        "AC6 Soft/Off zero-cost via production gate",
        ev_fiber_mut,
    )
    # No Soft treated as vulnerability: Soft path observability is
    # maintained (the counter still bumps) but the message is delivered
    # as before (AC3, AC2).

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3111 mailbox held_ref post-steal revalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
