#!/usr/bin/env python3
"""Issue #4158 source-cite gate: P0 edge-free outermost hold starvation
(bounded quarantine + structured admit refuse).

Under the production multi-worker latch, an outermost MutationBoundaryGuard
body that never hits a cooperative / JIT / eval_flat /
aura_jit_poll_hold_budget_safepoint edge kept workspace_mtx_ past the 2×SLO
inbody window. The #3859 quarantine face only bumped a counter at 4× the
inbody bound — the quarantine was open-ended: join hung, residual sticky
stayed, steal RejectHard / mailbox Backpressure starved under Agent load
(the #3764/#3826 busy-path peer dispose rarely fires: every worker sits
inside its own body).

Fix contract:
  - the #3859 quarantine latch now escalates (inbody-window poll — the
    mailbox SLO poll face): foreign holder → dispose_no_edge_holder
    (Running → mark_reclaimed so join returns Reclaimed; else Cancel+Done)
    + residual sticky armed via the SSOT reader; same-fiber needs nothing
    (force_release already ran with the dual-restore + unlock arm). The
    foreign unique_lock is NEVER unlocked (#4032/#3826/#3859 contract).
  - both Guard admission chains (try_acquire AND try_acquire_for_region)
    refuse new mutates with the structured AdmissionRejected:
    edge-free-hold-unsupported reason while the face is live (quarantine
    fired AND a no-edge hold is still live), placed before the #2701
    budget gate. Soft / unlatched never fires; holder exit disarms.

ACs:
  AC1  fiber.cpp quarantine escalation: the #3859 counter-bump window
       cites #4158, wires dispose_no_edge_holder + the SSOT sticky reader
       behind the foreign-only guard (same-fiber force_release already
       ran); kMutationHoldBudgetNoEdgeQuarantineSloMultiple stays 4.
  AC2  evaluator_mutation_boundary.cpp: the structured refuse appears in
       BOTH admission chains, each BEFORE its chain's #2701 budget gate,
       gated on latch + quarantine total + the live no-edge probe.
  AC3  Foreign-thread no-unlock contract intact: dispose stays
       unlock-free (#3764 face), force_release foreign arm unchanged
       (#3826 face), #3859 cite intact — no quarantine/sticky weakening.
  AC4  Wiring + no invent: linter registered in build.py and listed in
       scripts/coverage/root_check_allowlist.txt; no
       tests/**/test_issue_4158.cpp (per #81934); no docs/design/4158-*
       (per #1655); no new counters (g_4158_*) and no new query key
       (schema-4158) anywhere on the touched surfaces.

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

    fiber = texts["fiber"]
    emb = texts["emb"]
    mhb = texts["mhb"]
    build = texts["build"]
    allow = texts["allow"]

    # ── AC1: quarantine-age escalation in the inbody-window poll ──
    q = fiber.find("g_hold_budget_no_edge_quarantine_total.fetch_add")
    must(q >= 0, "AC1: #3859 quarantine bump present in fiber.cpp")
    if q >= 0:
        win = fiber[q : q + 1600]
        must("Issue #4158" in win, "AC1: escalation cites #4158")
        must("dispose_no_edge_holder" in win, "AC1: quarantine-age foreign dispose wired")
        must(
            "steal_safety_production_residual_zero_v_read" in win,
            "AC1: residual sticky armed via SSOT reader (no second model)",
        )
        must(
            "!(cur && cur->id() == fid)" in win,
            "AC1: foreign-only guard (same-fiber force_release already ran)",
        )
    must(
        "kMutationHoldBudgetNoEdgeQuarantineSloMultiple = 4" in mhb,
        "AC1: #3859 quarantine SLO multiple unchanged",
    )

    # ── AC2: structured refuse in both admission chains, before budget ──
    key = "AdmissionRejected: edge-free-hold-unsupported"
    occ1 = emb.find(key)
    occ2 = emb.find(key, occ1 + 1) if occ1 >= 0 else -1
    must(occ1 >= 0 and occ2 >= 0, "AC2: structured refuse in BOTH admission chains")
    bud1 = emb.find("AdmissionRejected: mutation-hold-budget")
    bud2 = emb.find("AdmissionRejected: mutation-hold-budget", bud1 + 1) if bud1 >= 0 else -1
    must(bud1 >= 0 and bud2 >= 0, "AC2: #2701 budget gates still present")
    must(occ1 >= 0 and bud1 >= 0 and occ1 < bud1, "AC2: refuse precedes budget (chain 1)")
    must(occ2 >= 0 and bud2 >= 0 and occ2 < bud2, "AC2: refuse precedes budget (chain 2)")
    must(
        emb.count("hold_budget_no_edge_quarantine_total_v_read() != 0") >= 2,
        "AC2: quarantine-age predicate in both chains",
    )
    must(
        emb.count("aura_mutation_hold_no_edge_still_held() != 0") >= 2,
        "AC2: live-scoped predicate in both chains",
    )
    must(
        emb.count("aura_runtime_multi_worker_production_latched() != 0") >= 4,
        "AC2: latch-gated (2 new + 2 existing #3288 rows)",
    )
    # Code-shape row (format-robust: strip whitespace + quotes, like the
    # #2701 linter's must_key) — a mention inside a comment does NOT count,
    # so removing one chain's actual gate is detected even though the
    # chain-1 rationale comment cites the literal.
    norm = "".join(ch for ch in emb if not ch.isspace() and ch != '"')
    must(
        norm.count("std::string(AdmissionRejected:edge-free-hold-unsupported)") >= 2,
        "AC2: refuse CODE rows in both chains (comment mentions do not count)",
    )

    # ── AC3: foreign-thread no-unlock contract intact ──
    must(
        "Never drops unique_lock from this thread" in fiber,
        "AC3: dispose stays unlock-free (#3764 face)",
    )
    must(
        "foreign — re-arm cancel only" in emb,
        "AC3: force_release foreign arm unchanged (#3826 face)",
    )
    must("Issue #3859" in fiber, "AC3: #3859 quarantine face cite intact")

    # ── AC4: wiring + no invent ──
    must("check_edge_free_hold_4158" in build, "AC4: build.py wires the linter")
    must("check_edge_free_hold_4158.py" in allow, "AC4: root_check_allowlist row present")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4158.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4158.cpp (per #81934)",
    )
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4158-*")),
        "AC4: no docs/design/4158-* (per #1655)",
    )
    for name, t in (
        ("fiber.cpp", fiber),
        ("evaluator_mutation_boundary.cpp", emb),
        ("mutation_hold_budget.h", mhb),
    ):
        must("g_4158_" not in t, f"AC4: no new counters in {name}")
        must("schema-4158" not in t, f"AC4: no new query key in {name}")

    return failures


def main() -> int:
    texts = {
        "fiber": FIBER.read_text(),
        "emb": EMB.read_text(),
        "mhb": MHB.read_text(),
        "build": BUILD.read_text(),
        "allow": ALLOW.read_text(),
    }
    if "--self-test" in sys.argv[1:]:
        real = run_rows(texts)
        broken = dict(texts)
        # Break one row per surface: drop the escalation cite (fiber.cpp)
        # and the first structured-refuse CODE row (admission chain 1) —
        # removing the std::string( code shape, not the bare literal, so
        # the chain-1 rationale comment mention cannot mask the break.
        broken["fiber"] = texts["fiber"].replace("Issue #4158", "Issue #XXXX", 1)
        broken["emb"] = texts["emb"].replace('std::string("AdmissionRejected: edge-free-hold-unsupported")', "", 1)
        failed = run_rows(broken)
        ok = not real and bool(failed)
        print(
            f"check_edge_free_hold_4158 self-test: real-tree rows clean={not real}, broken-detection rows={len(failed)}"
        )
        if not failed:
            print("  self-test FAILED: checker did not detect the broken synthetic")
        for row in real:
            print(f"  real-tree FAIL: {row}")
        return 0 if ok else 1
    failures = run_rows(texts)
    if failures:
        print(f"\ncheck_edge_free_hold_4158: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_edge_free_hold_4158: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
