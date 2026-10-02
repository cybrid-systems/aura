#!/usr/bin/env python3
"""Issue #4142 source-cite gate: WAL append-fail / overflow-refuse must not
thin the mid forensic payload into a false-green suggested-next.

Under production fail-closed WAL stress, wal_overflow_ring_push (#3838)
refuses the overwrite and emits a compensating SE PostureObserve with
reason "overflow-refuse" (denied=false); guard post-persist amend paths
emit "mutation_wal_append_miss" Deny rows. The mid stays joinable on the
SE ring, but query:evolution-audit-decision's #3246 suggested-next fold
only inspected typed_outcome ∈ {2,3} and last_se_denied — a refuse-only
mid (denied=false) and a post-wrap all-miss mid folded to "ok" exactly
when durability was being tested (ring observe ≠ full WAL row).

ACs:
  AC1  typed_mutation_audit.h: EvolutionSuggestedNextInput gains one
       additive bit (wal_miss_refuse_evidence) and decide_evolution_
       suggested_next returns InspectDeny when it is set — AFTER the
       #4064 deny arm and AFTER the Soft / mid=0 short-circuits (Soft
       stays soft-observe; mid=0 stays none — never invent trail
       Success); cites #4142.
  AC2  evaluator_primitives_security.cpp (evolution-audit-decision
       handler): nin.wal_miss_refuse_evidence is computed from the
       already-loaded faces — the "mutation_wal_append_miss" /
       "overflow-refuse" row reasons, the #4118/#3877 durable join
       (forensic_mid_has_wal_append_miss), and the post-wrap all-miss
       arm (se-mid-miss + typed-trail-miss + wal-lookup-window-miss)
       while the #3838 wal_overflow_ring_wrap_refuse_total counter is
       live.
  AC3  Soft zero-cost: the computation is a single production/Full-
       guarded expression whose leading (A || B) && short-circuits, so
       Soft / WAL-off never reaches the forensic join; no file I/O in
       the block.
  AC4  No new query key / no new metrics bus / no new mid counter:
       suggested-next stays the single observable (no insert_kv row
       cites 4142; planned_keys stays 72); the runtime door extends the
       existing tests/core/test_audit_replay_join.cpp family
       (ac23_wal_miss_refuse_next_4142 defined and registered in
       run_test_audit_replay_join); no tests/**/test_issue_4142.cpp
       (per #81934); no docs/design/4142-* (per #1655); build.py wires
       this linter and scripts/coverage/root_check_allowlist.txt lists it.
  AC5  Fail-closed preserved: security_event_wal.hh wal_overflow_ring_push
       still bumps wal_overflow_ring_wrap_refuse_total under the
       wal_append_fail_closed_active() gate and returns false (no
       fail-open overwrite), and still emits the compensating
       "overflow-refuse" PostureObserve that keeps the refused mid
       joinable on the SE ring.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_security.cpp"
WAL_HH = ROOT / "src" / "core" / "security_event_wal.hh"
TST = ROOT / "tests" / "core" / "test_audit_replay_join.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    tmh = TMH.read_text()
    prim = PRIM.read_text()
    wal = WAL_HH.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: fold contract in typed_mutation_audit.h ────────────────
    struct_region = _between(tmh, "struct EvolutionSuggestedNextInput", "decide_evolution_suggested_next(")
    must(
        "bool wal_miss_refuse_evidence = false;" in struct_region,
        "AC1: EvolutionSuggestedNextInput gains wal_miss_refuse_evidence",
    )
    must("#4142" in tmh, "AC1: typed_mutation_audit.h cites #4142")
    must(
        tmh.find("return EvolutionSuggestedNext::SoftObserve;")
        < tmh.find("if (in.join_mid == 0)")
        < tmh.find("if (in.typed_outcome == 2")
        < tmh.find("if (in.wal_miss_refuse_evidence)"),
        "AC1: Soft + mid=0 short-circuits and the #4064 deny arm precede the arm",
    )
    arm_region = _between(tmh, "if (in.wal_miss_refuse_evidence)", "if (in.schedule_would_deny")
    must("EvolutionSuggestedNext::InspectDeny" in arm_region, "AC1: the bit alone folds InspectDeny (never ok)")

    # ── AC2: handler evidence computation ───────────────────────────
    must("#4142" in prim, "AC2: evaluator_primitives_security.cpp cites #4142")
    assign_region = _between(prim, "nin.wal_miss_refuse_evidence =", "decide_evolution_suggested_next(nin)")
    must(bool(assign_region), "AC2: nin.wal_miss_refuse_evidence assignment present")
    for needle, label in [
        ('"mutation_wal_append_miss"', "AC2: append-miss row reason feeds the fold"),
        ('"overflow-refuse"', "AC2: overflow-refuse row reason feeds the fold"),
        ("forensic_mid_has_wal_append_miss(", "AC2: #4118/#3877 durable join reused"),
        ("wal_overflow_ring_wrap_refuse_total()", "AC2: #3838 refuse counter arm"),
        ("se_mid_miss != 0", "AC2: se-mid-miss face in the all-miss arm"),
        ("typed_miss != 0", "AC2: typed-trail-miss face in the all-miss arm"),
        ("wal_lookup_window_miss != 0", "AC2: wal-lookup-window-miss face in the arm"),
    ]:
        must(needle in assign_region, label)

    # ── AC3: Soft zero-cost (short-circuit guard, no new I/O) ───────
    gate = assign_region.find("production_defaults_active() ||")
    join = assign_region.find("forensic_mid_has_wal_append_miss(")
    must(0 <= gate < join, "AC3: leading production/Full guard short-circuits before the join")
    must("AuditStrategy::Full" in assign_region, "AC3: Full hard face keeps the same gate")
    must("fopen" not in assign_region and "fread" not in assign_region, "AC3: no new file I/O in the evidence block")

    # ── AC4: no new key / no second bus / family door + wiring ──────
    must(
        re.search(r'insert_kv(_str)?\("[^"]*4142', prim) is None,
        "AC4: no new insert_kv key cites 4142 (suggested-next stays the observable)",
    )
    must("kEvolutionAuditDecisionPlannedKeys = 72" in prim, "AC4: planned_keys stays 72 (no hash capacity churn)")
    must(
        "static void ac23_wal_miss_refuse_next_4142()" in tst,
        "AC4: ac23_wal_miss_refuse_next_4142 defined in test_audit_replay_join.cpp",
    )
    must(
        "ac23_wal_miss_refuse_next_4142();"
        in _between(tst, "int run_test_audit_replay_join()", "#ifndef AURA_ISSUE_BATCH_MEMBER"),
        "AC4: ac23 registered in run_test_audit_replay_join",
    )
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4142.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4142.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4142-*")), "AC4: no docs/design/4142-* (per #1655)")
    must("check_wal_miss_refuse_next_4142.py" in build, "AC4: build.py registers the linter")
    must("check_wal_miss_refuse_next_4142.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    # ── AC5: fail-closed refuse preserved (#3838) ───────────────────
    push_region = _between(wal, "inline bool wal_overflow_ring_push", "wal_overflow_ring_depth()")
    must(
        "wal_append_fail_closed_active()" in push_region,
        "AC5: refuse path still gated on wal_append_fail_closed_active()",
    )
    refuse_bump = push_region.find("wal_overflow_ring_wrap_refuse_total().fetch_add(1, std::memory_order_relaxed);")
    refuse_ret = push_region.find("return false;")
    store = push_region.find("= rec;")
    must(
        0 <= refuse_bump < refuse_ret < store, "AC5: fail-closed refuse still returns false before any overwrite store"
    )
    must(
        "append_security_event(" in push_region and "overflow-refuse" in push_region,
        "AC5: compensating overflow-refuse PostureObserve still emitted",
    )

    # Issue #4283 is the same residual as #4142, filed against an older
    # tip. The three Agent queries already expose the faces; do not add a
    # key, a second suggested-next, or a trail Success for mid=0.
    must("Issue #4142 / #4283" in tmh, "4283: fold cites #4283 beside #4142")
    must("query:security-audit" in tmh and "query:security-posture" in tmh, "4283: cites the three query faces")
    must(
        'insert_kv(\n                    "wal-overflow-wrap-refuse-total"' in prim
        or '"wal-overflow-wrap-refuse-total"' in prim,
        "4283: security-posture exposes wal-overflow-wrap-refuse-total",
    )
    must('"wal-append-fail-total"' in prim, "4283: security-posture exposes wal-append-fail-total")
    must(
        "query:security-audit" in prim and "filt_reason" in prim,
        "4283: security-audit keeps the reason filter",
    )
    for needle, label in [
        ("4142 AC1: refuse-only mid folds suggested-next=inspect-deny", "4283: AC1 inspect-deny"),
        ("4142 AC2: security-audit mutation-id filter shows the miss row", "4283: AC2 audit row"),
        ("forensic_mid_has_wal_append_miss(4142002)", "4283: AC2 forensic join"),
        ("4142 AC3: wrap-refuse counter bumped", "4283: AC3 wrap-refuse"),
        ("4142 AC3: older overflow mid preserved", "4283: AC3 no overwrite"),
        ("4142 AC4: post-wrap all-miss + live refuse counter stays inspect-deny", "4283: AC4 no false-green"),
        ("4142 AC5: Soft fold pushes nothing", "4283: AC5 Soft zero extra"),
        ("4142 AC5: mid=0 stays none", "4283: AC5 mid=0 none"),
    ]:
        must(needle in tst, label)
    must("query:4283" not in prim and "query:4283" not in tmh, "4283: no new query key")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4283.cpp"), recursive=True),
        "4283: no tests/**/test_issue_4283.cpp",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4283-*")), "4283: no docs/design/4283-*")

    if failures:
        print(f"\ncheck_wal_miss_refuse_next_4142: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_wal_miss_refuse_next_4142: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
