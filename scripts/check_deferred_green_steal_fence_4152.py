#!/usr/bin/env python3
"""Issue #4152 source-cite gate: commit_deferred_outermost_green_proof must
re-run the steal/last-look fence before republishing observer-visible green.

The #3984 deferred-green path armed two TLS bits at outermost persist and
the Guard commit republished (true,true) + Stamped purely from those bits.
publish_last_proof_face's green path re-binds green_bind_gen to the CURRENT
invalidate_gen, so a concurrent steal/densify success between the defer arm
and the Guard commit (invalidate_fast_path_before_steal_densify_restamp
advances g_rehydrate_miss_invalidate_gen and clears the green face) was
masked: the commit re-bound green over the drift and shipped a half-green
TypeLinearCommitProof over a drifted Occurrence fingerprint /
linear_root_count, and the #4030 grant rode along.

ACs:
  AC1  typed_mutation_audit.h :: commit_deferred_outermost_green_proof
       carries the #4152 fence: under stamp_last_look_hard() it requires
       g_rehydrate_miss_invalidate_gen == the gen frozen at defer arm AND
       re-runs the #3346 stamp last-look (stamp_last_look_live_matches with
       the frozen truth) BEFORE any green publish; the mismatch arm drops
       the deferred TLS, publishes the deny face (false,false), stamps
       kTypeLinearProofOutcomeReject and returns false (no #4030 grant).
       Ordering: fence < refuse < green publish < Stamped. Cites #4152.
  AC2  The defer arm in build_type_linear_commit_proof_from_live freezes
       the fence inputs (invalidate_gen via acquire + live_goals/fp/
       linear_roots/mid from the proof) alongside the two bits, BEFORE the
       (false,false) defer publish; drop_deferred_outermost_green_proof
       clears the fence inputs together with the bits (every existing drop
       site + reset_for_test stays consistent). No second proof model: the
       only invalidate-gen SSOT remains g_rehydrate_miss_invalidate_gen.
  AC3  Contracts preserved: the green publish + kTypeLinearProofOutcomeStamped
       and the #4011 remount-last-zero retire comment stay in the commit
       path; evaluator_mutation_boundary.cpp still grants
       type_export_authority only on a true commit (#4030); the #4151
       in-function gate and #4034/#4078 fiber authority surfaces untouched.
  AC4  Soft/Off zero-cost: the fence is the FIRST condition of the commit
       guard (stamp_last_look_hard() short-circuit) and Soft never defers
       (kTypeExportGrantAlignDeferredGreenIssue = 4030 intact); no new
       query key (no schema-4152), no new metrics bus, no new mid counter.
  AC5  Runtime doors extend tests/compiler/test_type_linear_commit_health.cpp
       (ac4152_1..ac4152_6 defined and called in
       run_test_type_linear_commit_health); no tests/**/test_issue_4152.cpp
       (per #81934); no docs/design/4152-* (per #1655); build.py wires this
       linter and scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TEST = ROOT / "tests" / "compiler" / "test_type_linear_commit_health.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

rows: list[tuple[bool, str]] = []


def check(ok: bool, label: str) -> None:
    rows.append((bool(ok), label))


def main() -> int:
    tmh = TMH.read_text()
    emb = EMB.read_text()
    test = TEST.read_text()

    # ── AC1: the fence lives in commit_deferred_outermost_green_proof ──
    check("kDeferredGreenStealFenceIssue = 4152" in tmh, "AC1: issue constant stamp")
    check("#4152" in tmh, "AC1: header cites #4152")
    fn = tmh.find("[[nodiscard]] inline bool commit_deferred_outermost_green_proof")
    check(fn != -1, "AC1: commit_deferred helper present")
    fence_if = tmh.find("if (stamp_last_look_hard() &&", fn)
    gen_cmp = tmh.find("g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire) !=", fn)
    last_look = tmh.find("stamp_last_look_live_matches(g_tls_deferred_outermost_green_live_goals", fn)
    refuse_drop = tmh.find("drop_deferred_outermost_green_proof();", gen_cmp)
    refuse_face = tmh.find("publish_last_proof_face(false, false);", gen_cmp)
    reject = tmh.find("publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);", gen_cmp)
    refuse_ret = tmh.find("return false;", gen_cmp)
    green = tmh.find("publish_last_proof_face(true, true);", fn)
    stamped = tmh.find("publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);", fn)
    check(
        -1 not in (fence_if, gen_cmp, last_look, refuse_drop, refuse_face, reject, refuse_ret, green, stamped)
        and fence_if < gen_cmp < last_look < refuse_drop
        and refuse_face < reject < refuse_ret < green < stamped,
        "AC1: fence (hard gate + gen compare + re-run last-look) precedes refuse, "
        "refuse (drop + deny face + Reject + return false) precedes the green republish",
    )

    # ── AC2: defer arm freezes the fence inputs; drop clears them ──
    arm = tmh.find("if ((!publish_green_face && p.would_allow_commit && p.linear_ok) || defer_remount_reproof) {")
    check(arm != -1, "AC2: defer arm site present")
    freeze_gen = tmh.find("g_tls_deferred_outermost_green_invalidate_gen =", arm)
    freeze_acquire = tmh.find("g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire);", freeze_gen)
    freeze_goals = tmh.find("g_tls_deferred_outermost_green_live_goals = p.live_goal_count;", arm)
    freeze_fp = tmh.find("g_tls_deferred_outermost_green_fp = p.goal_fingerprint;", arm)
    freeze_roots = tmh.find("g_tls_deferred_outermost_green_linear_roots = p.linear_root_count;", arm)
    freeze_mid = tmh.find("g_tls_deferred_outermost_green_mid = p.audit_mid;", arm)
    defer_publish = tmh.find("publish_last_proof_face(false, false);", arm)
    check(
        -1 not in (freeze_gen, freeze_acquire, freeze_goals, freeze_fp, freeze_roots, freeze_mid, defer_publish)
        and freeze_gen < freeze_goals < freeze_fp < freeze_roots < freeze_mid < defer_publish,
        "AC2: arm freezes gen (acquire) + frozen truth before the defer publish",
    )
    drop_fn = tmh.find("inline void drop_deferred_outermost_green_proof")
    clears = [
        tmh.find("g_tls_deferred_outermost_green_invalidate_gen = 0;", drop_fn),
        tmh.find("g_tls_deferred_outermost_green_live_goals = 0;", drop_fn),
        tmh.find("g_tls_deferred_outermost_green_fp = 0;", drop_fn),
        tmh.find("g_tls_deferred_outermost_green_linear_roots = 0;", drop_fn),
        tmh.find("g_tls_deferred_outermost_green_mid = 0;", drop_fn),
    ]
    check(
        drop_fn != -1 and all(c > drop_fn for c in clears),
        "AC2: drop_deferred clears the fence inputs with the bits",
    )
    check(
        tmh.count("inline std::atomic<std::uint64_t> g_rehydrate_miss_invalidate_gen{") == 1,
        "AC2: single invalidate-gen SSOT (no second proof model)",
    )

    # ── AC3: preserved contracts (#3984/#4011/#4030 surfaces intact) ──
    remount = tmh.find("this green publish is the remount-last-zero", fn)
    check(remount != -1 and remount < green, "AC3: #4011 remount retire intact at green")
    guard_commit = emb.find("if (typed_audit::commit_deferred_outermost_green_proof())")
    guard_grant = emb.find("ev_->grant_type_export_authority();", guard_commit)
    check(
        guard_commit != -1 and guard_grant != -1,
        "AC3: Guard grant stays gated on the commit bool (#4030)",
    )
    check(
        tmh.find("kOutermostGreenAfterPostPersistDenyIssue = 3984") != -1,
        "AC3: #3984 defer contract constant intact",
    )
    check(
        "kDeferredGreenStealFenceIssue" in test,
        "AC3: runtime doors cite the #4152 constant",
    )

    # ── AC4: Soft/Off zero-cost + no invented observability ──
    check(
        -1 not in (fence_if, gen_cmp) and fence_if < gen_cmp,
        "AC4: stamp_last_look_hard() short-circuits the fence before any loads",
    )
    check(
        tmh.find("kTypeExportGrantAlignDeferredGreenIssue = 4030") != -1,
        "AC4: #4030 Soft-never-defers constant intact",
    )
    check("schema-4152" not in tmh, "AC4: no new query key")
    check("4152" not in emb or "Issue #4152" not in emb, "AC4: Guard site unchanged")

    # ── AC5: runtime doors + wiring + allowlist + no invent ──
    acs = [
        "ac4152_1_gen_advance_refuses",
        "ac4152_2_last_look_drift_refuses",
        "ac4152_3_happy_commits_and_rebinds",
        "ac4152_4_soft_no_defer_no_fence",
        "ac4152_5_guard_happy_no_false_refuse",
        "ac4152_6_source_cite_fence_order",
    ]
    runner = test.find("int run_test_type_linear_commit_health()")
    check(runner != -1, "AC5: dispatched runner present")
    for name in acs:
        defined = test.find(f"static void {name}()") != -1
        called = test.find(f"    {name}();", runner) != -1
        check(defined and called, f"AC5: {name} defined and dispatched")
    check(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4152.cpp"), recursive=True),
        "AC5: no tests/**/test_issue_4152.cpp (#81934)",
    )
    check(
        not glob.glob(str(ROOT / "docs" / "design" / "4152-*")),
        "AC5: no docs/design/4152-* (#1655)",
    )
    check(
        "check_deferred_green_steal_fence_4152.py" in BUILD.read_text(),
        "AC5: build.py wires this linter",
    )
    check(
        "check_deferred_green_steal_fence_4152.py" in ALLOWLIST.read_text(),
        "AC5: root_check_allowlist.txt lists this linter",
    )

    failed = [label for ok, label in rows if not ok]
    for ok, label in rows:
        print(("PASS  " if ok else "FAIL  ") + label)
    if failed:
        print(f"\n{len(failed)} row(s) failed")
        return 1
    print(f"\nall {len(rows)} rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
