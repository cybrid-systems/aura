#!/usr/bin/env python3
"""Issue #4160 source-cite gate: P1 mailbox hold p99 denies new mutate but
does not bound holder unlock.

The mailbox under-boundary wait p99 / starvation throttle correctly deny NEW
try_acquire via security_schedule_gate (mailbox_hold_slo) and the one-shot
request_hold_budget_cancel — but the deny face only drove
aura_hold_budget_poll_inbody_window, whose #3859/#4158 quarantine-dispose
ladder is multi-worker-latch-gated. A holder that never polls a cooperative
edge kept workspace_mtx_ while receivers Backpressured and new Agents got
AdmissionRejected — livelock of the fleet, not silent half-topology.

Fix contract (existing mechanisms only — #2958 face, #3223 nudge,
#3764/#3826/#3859/#4158 dispose, #3162/#3195 sticky bus):
  - the one-shot cancel arm in maybe_mailbox_defer_slo_hold_cancel is ALWAYS
    paired with aura_fiber_request_urgent_inbody_poll(holder) so the
    victim's next cooperative edge matches same-fiber force-release, and
    with the peer aura_hold_budget_poll_busy_path() so an edge-free holder
    is bounded (same-fiber force-release; foreign → residual sticky +
    dispose_no_edge_holder Reclaimed/Done) instead of only ever re-armed;
  - the already-armed re-poll (#3289) drives the busy-path bound too;
  - the #3859 quarantine latch, observed hot from THIS face, arms the same
    residual sticky admit deny via the SSOT reader (steal_safety_
    production_residual_zero_v_read) — soak/admit deny (incl. the #4158
    structured edge-free refuse) within one mailbox SLO poll; Soft /
    non-production stays observe-only (tie sits after the soft return).

ACs:
  AC1  multi_fiber_mailbox.h: the #3256 arm window pairs the urgent inbody
       poll + the peer busy-path bound and cites #4160; the #3289 re-poll
       window drives the busy-path bound too; the #3859 tie-in sits AFTER
       the Soft observe-only return and arms the sticky via the SSOT
       reader (no second model, no new counters).
  AC2  The #2958 SSOT face is unchanged: sample_mailbox_hold_slo_live feeds
       the cancel, the one-shot arm CAS + kMailboxDeferSloHoldCancelIssue =
       2958 stay intact (no cancel-storm model change).
  AC3  Foreign-thread no-unlock contract intact: dispose stays unlock-free
       (#3764 face), force_release foreign arm unchanged (#3826 face),
       #4158 quarantine escalation + #3859 SLO multiple intact.
  AC4  Wiring + no invent: linter registered in build.py and listed in
       scripts/coverage/root_check_allowlist.txt; no
       tests/**/test_issue_4160.cpp (per #81934); no docs/design/4160-*
       (per #1655); no new counters (g_4160_*) and no new query key
       (schema-4160) anywhere on the touched surfaces.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
--self-test runs the rows against the real tree (must be clean) AND
against a broken synthetic (must be detected) to prove the checker fails
when the contract is violated.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MB = ROOT / "src" / "serve" / "multi_fiber_mailbox.h"
FIBER = ROOT / "src" / "serve" / "fiber.cpp"
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
MHB = ROOT / "src" / "compiler" / "mutation_hold_budget.h"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def run_rows(texts: dict) -> list[str]:
    failures: list[str] = []

    def must(cond: bool, row: str) -> None:
        if not cond:
            failures.append(row)

    mb = texts["mb"]
    fiber = texts["fiber"]
    emb = texts["emb"]
    mhb = texts["mhb"]
    build = texts["build"]
    allow = texts["allow"]

    # ── AC1: the deny face pairs urgent poll + busy-path bound + sticky tie ──
    arm = mb.find("Issue #3256: unify mailbox under-boundary SLO")
    must(arm >= 0, "AC1: #3256 arm window present in multi_fiber_mailbox.h")
    if arm >= 0:
        win = mb[arm : arm + 1600]
        must("Issue #4160" in win, "AC1: arm window cites #4160")
        must(
            "aura_fiber_request_urgent_inbody_poll(holder.fiber_id)" in win,
            "AC1: urgent inbody poll paired with the cancel arm",
        )
        must(
            "aura_hold_budget_poll_busy_path()" in win,
            "AC1: peer busy-path bound paired with the cancel arm",
        )
    rp = mb.find("Issue #3289 (I5 residual)")
    must(rp >= 0, "AC1: #3289 re-poll branch present")
    if rp >= 0:
        rwin = mb[rp : rp + 1600]
        must("aura_hold_budget_poll_inbody_window()" in rwin, "AC1: re-poll inbody drive intact")
        must(
            "aura_hold_budget_poll_busy_path()" in rwin,
            "AC1: re-poll drives the busy-path bound",
        )
    soft = mb.find("mailbox_defer_slo_soft_observe_total.fetch_add")
    tie = mb.find("g_hold_budget_no_edge_quarantine_latched.load")
    must(soft >= 0 and tie >= 0 and soft < tie, "AC1: quarantine tie-in after Soft observe-only")
    must(
        "Issue #4160: tie the #3859 quarantine SLO" in mb,
        "AC1: tie-in cites #4160",
    )
    if tie >= 0:
        twin = mb[tie : tie + 300]
        must(
            "steal_safety_production_residual_zero_v_read" in twin,
            "AC1: sticky armed via the SSOT reader (no second model)",
        )
    must(
        "aura::serve::steal_safety_production_residual_zero_v_read()" in mb,
        "AC1: SSOT reader call is the #3134/#3195 accessor (qualified)",
    )

    # ── AC2: #2958 SSOT face unchanged ──
    must(
        "Issue #2958: under-boundary wait / open-window age" in mb,
        "AC2: #2958 SLO cancel contract header intact",
    )
    must(
        "inline constexpr int kMailboxDeferSloHoldCancelIssue = 2958;" in mb,
        "AC2: one-shot arm issue constant unchanged",
    )
    must(
        mb.count("maybe_mailbox_defer_slo_hold_cancel") >= 5,
        "AC2: #2958 helper still wired (decl + def + call sites)",
    )
    must(
        "sample_mailbox_hold_slo_live(p99, throttled, slo)" in mb,
        "AC2: SSOT live sample still feeds the cancel (#3002 face)",
    )

    # ── AC3: foreign no-unlock + prior contracts intact ──
    must(
        "Never drops unique_lock from this thread" in fiber,
        "AC3: dispose stays unlock-free (#3764 face)",
    )
    must("foreign — re-arm cancel only" in emb, "AC3: force_release foreign arm unchanged")
    must("Issue #4158" in fiber, "AC3: #4158 quarantine escalation intact")
    q = fiber.find("g_hold_budget_no_edge_quarantine_total.fetch_add")
    must(q >= 0, "AC3: #3859 quarantine bump present")
    if q >= 0:
        qwin = fiber[q : q + 1600]
        must("dispose_no_edge_holder" in qwin, "AC3: quarantine-age dispose wired (#4158)")
        must(
            "steal_safety_production_residual_zero_v_read" in qwin,
            "AC3: #4158 sticky arm via SSOT reader intact",
        )
    must(
        "kMutationHoldBudgetNoEdgeQuarantineSloMultiple = 4" in mhb,
        "AC3: #3859 quarantine SLO multiple unchanged",
    )

    # ── AC4: wiring + no invent ──
    must("check_mailbox_holder_bound_4160" in build, "AC4: build.py wires the linter")
    must("check_mailbox_holder_bound_4160.py" in allow, "AC4: root_check_allowlist row present")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4160.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4160.cpp (per #81934)",
    )
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4160-*")),
        "AC4: no docs/design/4160-* (per #1655)",
    )
    for name, t in (
        ("multi_fiber_mailbox.h", mb),
        ("fiber.cpp", fiber),
        ("evaluator_mutation_boundary.cpp", emb),
        ("mutation_hold_budget.h", mhb),
    ):
        must("g_4160_" not in t, f"AC4: no new counters in {name}")
        must("schema-4160" not in t, f"AC4: no new query key in {name}")

    return failures


def main() -> int:
    texts = {
        "mb": MB.read_text(),
        "fiber": FIBER.read_text(),
        "emb": EMB.read_text(),
        "mhb": MHB.read_text(),
        "build": BUILD.read_text(),
        "allow": ALLOW.read_text(),
    }
    if "--self-test" in sys.argv[1:]:
        real = run_rows(texts)
        broken = dict(texts)
        # Break one row per surface: drop the #4160 cite from the arm window
        # and drop the FIRST busy-path pairing call (the arm path) so the
        # arm-window row AND the tie window both lose their anchor.
        broken["mb"] = (
            texts["mb"]
            .replace("Issue #4160", "Issue #XXXX", 1)
            .replace("    (void)aura_hold_budget_poll_busy_path();", "", 1)
        )
        failed = run_rows(broken)
        ok = not real and bool(failed)
        print(
            f"check_mailbox_holder_bound_4160 self-test: real-tree rows clean={not real}, "
            f"broken-detection rows={len(failed)}"
        )
        if not failed:
            print("  self-test FAILED: checker did not detect the broken synthetic")
        for row in real:
            print(f"  real-tree FAIL: {row}")
        return 0 if ok else 1
    failures = run_rows(texts)
    if failures:
        print(f"\ncheck_mailbox_holder_bound_4160: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_mailbox_holder_bound_4160: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
