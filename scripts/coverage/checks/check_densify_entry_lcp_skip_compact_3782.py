#!/usr/bin/env python3
"""Issue #3782: densify-entry LCP reject skips compact_all_moving_pinned.

#3185 wired densify-entry LCP consult but left a post-move success-gate
poison: Phase-5 and sticky-recovery still called compact_all_moving_pinned
then forced pin_contract_held=false. Objects could relocate under a
stamped-reject LCP before the densify face failed.

#3782: on stamped reject, SKIP compact (objects_moved==0), publish a
blocked window, fail the densify face. Consult is eval-keyed (#3617 slots,
#3634 shape) so Eval A's reject does not incorrectly block Eval B when B
has its own stamped slot.

Contract:
  AC1 Phase-5: consult eval-keyed; on reject, skip compact_all_moving_pinned
      (guarded); pin_contract_held forced false; objects_moved stays 0
  AC2 sticky-recovery one-shot: same skip-then-fail order (no relocate-
      then-poison); publishes blocked window when skipped
  AC3 consult_last_lcp_for_densify_entry takes eval_id; per-Eval first
      (#3617 present_for / would_allow_for) then process-wide fallback
  AC4 Soft/Off: production_defaults_active || Full guard unchanged;
      evaluator_gc.cpp still has no LCP consult
  AC5 tests extend test_moving_densify_fail_closed (no test_issue_3782.cpp);
      no docs/design/3782-*; build.py wires this linter after #3781

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    lcp = _read("src/core/lifetime_consistency_proof.hh")
    gc = _read("src/compiler/evaluator_gc.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1 Phase-5 skip-compact ──
    phase5_anchor = "Issue #3185 AC1: consult last LifetimeConsistencyProof before"
    p5 = mut.find(phase5_anchor)
    p5w = mut[p5 : p5 + 4500] if p5 != -1 else ""
    must("Issue #3782", "AC1 Phase-5 cite", p5w)
    must("consult_last_lcp_for_densify_entry(static_cast<const void*>(ev_))", "AC1 eval-keyed", p5w)
    must("if (!densify_entry_lcp_blocked)", "AC1 skip guard", p5w)
    must("compact_all_moving_pinned()", "AC1 compact under guard", p5w)
    must("compact_r.pin_contract_held = false", "AC1 synthetic fail-closed", p5w)
    must(
        "compact_r.pin_contract_held && !densify_entry_lcp_blocked",
        "AC1 pin force",
        p5w,
    )
    # Order: skip guard before the live compact call (ignore comment mention).
    skip_at = p5w.find("if (!densify_entry_lcp_blocked)")
    call_at = p5w.find("ev_->arena_group_->compact_all_moving_pinned()")
    if skip_at == -1 or call_at == -1 or skip_at > call_at:
        fails.append("AC1: Phase-5 must guard compact_all_moving_pinned behind !densify_entry_lcp_blocked")
    # Publish blocked attempt (had || densify_entry_lcp_blocked)
    must("publish_had = had_moving_densify || densify_entry_lcp_blocked", "AC1 publish_had", mut)

    # ── AC2 sticky-recovery ──
    oneshot_anchor = "Issue #3185 AC1: same surface as Phase-5 densify entry"
    o5 = mut.find(oneshot_anchor)
    o5w = mut[o5 : o5 + 4500] if o5 != -1 else ""
    must("Issue #3782", "AC2 recovery cite", o5w)
    must(
        "consult_last_lcp_for_densify_entry(static_cast<const void*>(this))",
        "AC2 eval-keyed",
        o5w,
    )
    must("if (densify_entry_lcp_blocked)", "AC2 skip branch", o5w)
    must("publish_last_moving_densify_window", "AC2 blocked publish", o5w)
    # compact only in else of densify_entry_lcp_blocked
    skip_branch = o5w.find("if (densify_entry_lcp_blocked)")
    else_compact = o5w.find("arena_group_->compact_all_moving_pinned()", skip_branch if skip_branch != -1 else 0)
    if skip_branch == -1 or else_compact == -1 or else_compact < skip_branch:
        fails.append("AC2: recovery compact_all_moving_pinned must sit after densify_entry_lcp_blocked skip branch")
    pre = mut[max(0, o5 - 3000) : o5] if o5 != -1 else ""
    must("Evaluator::recover_moving_sticky_densify_off", "AC2 in recover", pre)

    # ── AC3 per-Eval consult ──
    must("Issue #3782", "AC3 cite", lcp)
    must("consult_last_lcp_for_densify_entry(const void* eval_id", "AC3 signature", lcp)
    must("last_lifetime_consistency_proof_present_for(eval_id)", "AC3 keyed present", lcp)
    must("last_lifetime_consistency_would_allow_for(eval_id)", "AC3 keyed allow", lcp)
    must("last_lifetime_consistency_proof_present()", "AC3 process-wide fallback", lcp)
    must("#3617", "AC3 cites #3617", lcp)

    # ── AC4 Soft/Off + gc unchanged ──
    must("typed_audit::production_defaults_active() ||", "AC4 Soft guard Phase-5", p5w)
    must("typed_audit::production_defaults_active() ||", "AC4 Soft guard recovery", o5w)
    must_not("consult_last_lcp_for_densify_entry", "AC4 gc no consult", gc)
    must_not("Issue #3782", "AC4 gc no cite", gc)

    # ── AC5 tests / wiring / no invent ──
    must("Issue #3782", "AC5 test cite", test)
    must("ac3782_1", "AC5 ac3782_1", test)
    must("ac3782_2", "AC5 ac3782_2", test)
    must("ac3782_3", "AC5 ac3782_3", test)
    must("check_densify_entry_lcp_skip_compact_3782", "AC5 build wire", build)
    if build.find("check_densify_entry_lcp_skip_compact_3782") <= build.find("check_densify_this_window_rewrite_3781"):
        fails.append("AC5: linter must be wired in build.py AFTER #3781 linter")
    must_not("tests/issues/test_issue_3782.cpp", "AC5 no issue test path invent", test)
    if (ROOT / "tests" / "issues" / "test_issue_3782.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3782.cpp present (forbidden)")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3782-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_phase5_skip_compact")
    print("OK  AC2_sticky_recovery_skip")
    print("OK  AC3_eval_keyed_consult")
    print("OK  AC4_soft_off_gc_unchanged")
    print("OK  AC5_tests_wiring_no_invent")
    print(
        "\nOK: Issue #3782 densify-entry LCP skip-compact — Phase-5 + sticky-recovery "
        "skip relocate on reject; eval-keyed consult; Soft zero-cost"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
