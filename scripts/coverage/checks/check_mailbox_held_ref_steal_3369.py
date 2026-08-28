#!/usr/bin/env python3
"""Issue #3369: [orch/mailbox] #3111 residual — wire the production
held_ref post-steal walk (revalidate_held_ref_after_steal was counter-
only; bump_held_ref_stale_after_steal had zero call sites outside its
definition).

Contract (one row per AC):
  AC1  Production + steal-complete of a fiber attached to a mailbox →
       clear handoff_completed per stale held_ref message + bump
       held_ref_stale_after_steal_total. Walk runs under the mailbox
       mutex already used by push/recv. for_each_pending_held_ref_for_
       fiber on MultiFiberMailbox is the walk entry point.
  AC2  Walk gated on production_defaults_active() (Soft / sandbox=off:
       counter bumps only, may still deliver — preserves #3111 AC3).
  AC3  handoff_completed cleared per stale message; push-time gate
       (#2663 / #3013 / #3212) still rejects on push when held_ref_token
       is set but handoff_completed is false. Quiet path: walk returns
       quickly when queue is empty / no held_ref messages.
  AC4  Additive counters only (held_ref_post_steal_check_total +
       held_ref_stale_after_steal_total — both pre-existing from #3111).
       No new query key. No Soft treated as vulnerability.
  AC5  Fiber → mailbox direction maintained via a back-pointer on
       Fiber (mf_mailbox::MultiFiberMailbox* mailbox_) — NOT a new
       process-global AgentRegistry. MultiFiberMailbox::attach /
       detach maintain the pointer under their existing mu_ critical
       section.
  AC6  No docs/design/3369-* per #1655. No tests/serve/test_issue_3369.cpp
       per #81967 / R17 — extend the existing
       tests/serve/test_steal_complete_restamp_txn.cpp suite (the
       #3111 AC5 pattern) and add AC1–AC6 markers inside the existing
       run_test_ scope. Linter wired in build.py. Dummy EnvFrame
       handoff_ref stub at ~evaluator_fiber_mutation.cpp:3992-4019 is a
       separate follow-up and is NOT touched.

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
    fiberh = _read("src/serve/fiber.h")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # ── AC1: post-steal walk wired ──────────────────────────────────────────
    # Walk function on MultiFiberMailbox.
    must("for_each_pending_held_ref_for_fiber", "AC1 walk function in multi_fiber_mailbox.h", mfbh)
    must(
        "std::lock_guard lock(mu_);",
        "AC1 walk holds mu_ (same lock as push/recv)",
        mfbh,
    )
    # Pre-existing #3111 primitives still present (the walk is the new piece).
    must("held_ref_post_steal_check_total", "AC1 pre-existing #3111 check counter in multi_fiber_mailbox.h", mfbh)
    must("held_ref_stale_after_steal_total", "AC1 pre-existing #3111 stale counter in multi_fiber_mailbox.h", mfbh)
    must("revalidate_held_ref_after_steal()", "AC1 pre-existing #3111 revalidate helper in multi_fiber_mailbox.h", mfbh)
    must("bump_held_ref_stale_after_steal()", "AC1 pre-existing #3111 bump helper in multi_fiber_mailbox.h", mfbh)
    # Walk invoked from steal-complete strong def.
    must(
        "for_each_pending_held_ref_for_fiber",
        "AC1 walk invoked from evaluator_fiber_mutation.cpp (steal-complete strong def)",
        ev_fiber_mut,
    )
    must(
        "bump_held_ref_stale_after_steal()",
        "AC1 bump helper called from walk callback in evaluator_fiber_mutation.cpp",
        ev_fiber_mut,
    )
    must(
        "aura_evaluator_on_steal_complete",
        "AC1 strong def context (steal-complete entry point)",
        ev_fiber_mut,
    )
    must("3369 AC1", "AC1 test marker", test)

    # ── AC2: production-gated walk (Soft / sandbox=off zero-cost) ──────────
    must(
        "if (aura::compiler::typed_audit::production_defaults_active() && fiber_ptr) {",
        "AC2 walk gated on production + fiber_ptr (Soft/Off zero-cost)",
        ev_fiber_mut,
    )
    must(
        "typed_audit::production_defaults_active()",
        "AC2 production gate preserved",
        ev_fiber_mut,
    )
    must("3369 AC2", "AC2 test marker", test)

    # ── AC3: handoff_completed cleared per stale message; push gate unchanged ─
    must(
        "m.handoff_completed = false",
        "AC3 handoff_completed cleared per stale held_ref message",
        ev_fiber_mut,
    )
    must(
        "if (msg.held_ref_token.has_value() && !msg.handoff_completed)",
        "AC3 push-time held_ref gate unchanged",
        mfbh,
    )
    must(
        "handoff_reject_total",
        "AC3 push-time handoff_reject counter preserved",
        mfbh,
    )
    must("3369 AC3", "AC3 test marker", test)

    # ── AC4: additive counters only (no new query key) ─────────────────────
    must(
        "held_ref_post_steal_check_total",
        "AC4 additive check counter (pre-existing #3111)",
        mfbh,
    )
    must(
        "held_ref_stale_after_steal_total",
        "AC4 additive stale counter (pre-existing #3111)",
        mfbh,
    )
    must("3369 AC4", "AC4 test marker", test)

    # ── AC5: Fiber → mailbox back-pointer (no process-global AgentRegistry) ─
    # Forward declaration in fiber.h.
    must(
        "class MultiFiberMailbox;",
        "AC5 forward declaration of MultiFiberMailbox in fiber.h",
        fiberh,
    )
    # Fiber-side mailbox_ field + accessors.
    must(
        "mf_mailbox::MultiFiberMailbox* mailbox_",
        "AC5 Fiber::mailbox_ field in fiber.h",
        fiberh,
    )
    must(
        "mf_mailbox::MultiFiberMailbox* mailbox() const noexcept",
        "AC5 Fiber::mailbox() getter in fiber.h",
        fiberh,
    )
    must(
        "void set_mailbox(mf_mailbox::MultiFiberMailbox* m) noexcept",
        "AC5 Fiber::set_mailbox() setter in fiber.h",
        fiberh,
    )
    # MultiFiberMailbox::attach / detach maintain the back-pointer.
    must(
        "f->set_mailbox(this)",
        "AC5 MultiFiberMailbox::attach sets Fiber::mailbox_",
        mfbh,
    )
    must(
        "f->set_mailbox(nullptr)",
        "AC5 MultiFiberMailbox::detach clears Fiber::mailbox_",
        mfbh,
    )
    # Walk reads Fiber::mailbox() (caller side).
    must(
        "fiber->mailbox()",
        "AC5 steal-complete strong def reads Fiber::mailbox() to find the mailbox",
        ev_fiber_mut,
    )
    must("3369 AC5", "AC5 test marker", test)

    # ── AC6: no docs/design/, no test_issue_3369.cpp, linter wired ─────────
    must("3369 AC6", "AC6 test marker", test)
    must(
        "check_mailbox_held_ref_steal_3369",
        "AC6 build.py wires 3369 linter",
        build,
    )
    must("Issue #3369", "AC6 linter error message in build.py", build)
    if (ROOT / "tests" / "serve" / "test_issue_3369.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_3369.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3369.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3369.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3369-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    # No-invent: extend existing test (this file)
    test_self = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    must(
        "3369 AC1",
        "AC6 existing test file cites #3369",
        test_self,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3369 mailbox held_ref post-steal walk (production) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
