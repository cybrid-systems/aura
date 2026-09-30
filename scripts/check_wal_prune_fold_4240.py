#!/usr/bin/env python3
"""Issue #4240 source-cite gate: segment prune after ring wrap must not
make an audited mid look never-audited to the Agent fold.

Under production + force_wal, a mid can be dual-written to the SE ring +
SE/mutation WAL. Over a long run the typed trail wraps (256, #3113), the
SE ring wraps (1024, #2225), and AURA_WAL_MAX_SEGMENTS retention prunes
the only durable segment that held the mid (#3338). The #4142
wal_miss_refuse_evidence fold covered append-miss / overflow-refuse /
post-wrap all-miss while the #3838 refuse counter is live — but NOT the
prune-evicted all-miss, so query:evolution-audit-decision's
suggested-next folded a forensically thinned mid to "ok" exactly when
the durable evidence had been unlinked.

ACs:
  AC1  typed_mutation_audit.h: the #4142 fold contract the prune arm
       rides is intact — wal_miss_refuse_evidence still folds
       InspectDeny AFTER the #4064 deny arm and AFTER the Soft /
       mid=0 short-circuits (Soft stays soft-observe; mid=0 stays
       none — never invent trail Success); no header change.
  AC2  evaluator_primitives_security.cpp (evolution-audit-decision
       handler): nin.wal_miss_refuse_evidence gains the #4240
       prune-evicted all-miss arm — production + join_mid +
       se-mid-miss + typed-trail-miss + wal-lookup-window-miss AND
       (segment prune counters > 0 OR wal_full_scan_exhausted) —
       additive beside the preserved #4142 arms.
  AC3  wal_full_scan_exhausted face + observe reason: both exhaustive
       scan-all branches mark exhaustion; the durable miss block emits
       last-se-reason "wal-segment-pruned" (prune counters live)
       before the #3838 wrap-evicted fallback, with live overflow
       rows still first.
  AC4  Zero-cost + wiring: the prune counters are read lazily inside
       production/Full-gated arms (Soft / WAL-off / retention=0 keep
       the fold unchanged); the leading guard short-circuits before
       the forensic join; no new query key cites 4240; planned_keys
       stays 72; the runtime door extends
       tests/compiler/test_evolution_audit_decision_forensic.cpp
       (ac11_wal_prune_fold_4240 registered in main); no
       tests/**/test_issue_4240.cpp (per #81934); no
       docs/design/4240-* (per #1655); build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

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
TST = ROOT / "tests" / "compiler" / "test_evolution_audit_decision_forensic.cpp"
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


def _flat(text: str) -> str:
    return "".join(ch for ch in text if not ch.isspace())


def main() -> int:
    tmh = TMH.read_text()
    prim = PRIM.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: header fold contract the prune arm rides ───────────────
    must(
        "bool wal_miss_refuse_evidence = false;" in tmh,
        "AC1: wal_miss_refuse_evidence bit present (no header change)",
    )
    must("#4142" in tmh, "AC1: header still cites #4142 (fold lineage)")
    must(
        tmh.find("return EvolutionSuggestedNext::SoftObserve;")
        < tmh.find("if (in.join_mid == 0)")
        < tmh.find("if (in.typed_outcome == 2")
        < tmh.find("if (in.wal_miss_refuse_evidence)"),
        "AC1: Soft + mid=0 short-circuits and the #4064 deny arm precede the fold",
    )
    arm_region = _between(tmh, "if (in.wal_miss_refuse_evidence)", "if (in.schedule_would_deny")
    must(
        "EvolutionSuggestedNext::InspectDeny" in arm_region,
        "AC1: the bit alone folds InspectDeny (never ok — #4240 rides this)",
    )

    # ── AC2: the #4240 prune-evicted all-miss fold arm ──────────────
    must("#4240" in prim, "AC2: evaluator_primitives_security.cpp cites #4240")
    assign_region = _between(prim, "nin.wal_miss_refuse_evidence =", "decide_evolution_suggested_next(nin)")
    must(bool(assign_region), "AC2: nin.wal_miss_refuse_evidence assignment present")
    flat_assign = _flat(assign_region)
    for needle, label in [
        ("Issue#4240", "AC2: #4240 arm cited at the fold"),
        ("production_defaults_active()&&join_mid!=0", "AC2: prune arm gated production + join_mid"),
        ("se_mid_miss!=0", "AC2: se-mid-miss face in the prune arm"),
        ("typed_miss!=0", "AC2: typed-trail-miss face in the prune arm"),
        ("wal_lookup_window_miss!=0", "AC2: wal-lookup-window-miss face in the prune arm"),
        ("audit_wal_segment_prune_total", "AC2: mutation audit WAL prune counter arm"),
        ("security_event_wal_segment_prune_total", "AC2: SE WAL prune counter arm"),
        ("wal_full_scan_exhausted!=0", "AC2: full-scan-exhausted disjunct in the prune arm"),
        ('"mutation_wal_append_miss"', "AC2: #4142 append-miss arm preserved"),
        ('"overflow-refuse"', "AC2: #4142 overflow-refuse arm preserved"),
        ("forensic_mid_has_wal_append_miss(join_mid)", "AC2: #4118/#3877 durable join preserved"),
        ("wal_overflow_ring_wrap_refuse_total()", "AC2: #4142 refuse-counter arm preserved"),
    ]:
        must(needle in flat_assign, label)

    # ── AC3: exhausted face + wal-segment-pruned observe reason ─────
    must(
        "std::int64_t wal_full_scan_exhausted = 0;" in prim,
        "AC3: wal_full_scan_exhausted declared beside wal_full_scan_hit",
    )
    arms = [m.start() for m in re.finditer(re.escape("wal_full_scan_exhausted = 1"), prim)]
    must(len(arms) == 2, "AC3: both scan-all branches mark exhaustion (SE + mutation only)")
    se_all = prim.find("se_wal.find_by_mutation_id_scan_all_segments(join_mid)")
    mut_all = prim.find("find_by_provenance_mutation_id_scan_all_segments(join_mid)")
    must(
        bool(arms) and se_all >= 0 and mut_all >= 0 and arms[0] > se_all and arms[1] > mut_all,
        "AC3: each exhausted arm follows its exhaustive scan-all call",
    )
    miss_block = prim.find("if (durable_hit == 0 && last_se_reason_str.empty())")
    ovr = prim.find("wal_overflow_find_by_mid(", miss_block)
    pruned = prim.find('"wal-segment-pruned"', miss_block)
    wrap = prim.find('"overflow_wrap_evicted"', miss_block)
    must(miss_block >= 0, "AC3: durable miss reason block present")
    must(
        ovr >= 0 and pruned >= 0 and wrap >= 0,
        "AC3: overflow / wal-segment-pruned / wrap-evicted reason arms present",
    )
    must(0 <= ovr < pruned < wrap, "AC3: reason precedence row < prune < wrap-evicted")
    gate = prim.find("if ((want_durable || auto_durable) && join_mid != 0 &&")
    must(
        gate >= 0 and pruned > gate,
        "AC3: prune reason sits inside the production/Full durable block",
    )

    # ── AC4: zero-cost + no new key + wiring ────────────────────────
    must(
        re.search(r'insert_kv(_str)?\("[^"]*4240', prim) is None,
        "AC4: no new insert_kv key cites 4240 (suggested-next stays the observable)",
    )
    must(
        "kEvolutionAuditDecisionPlannedKeys = 72" in prim,
        "AC4: planned_keys stays 72 (no hash capacity churn)",
    )
    leading = assign_region.find("production_defaults_active() ||")
    join = assign_region.find("forensic_mid_has_wal_append_miss(")
    must(0 <= leading < join, "AC4: leading production/Full guard short-circuits before the join")
    must("AuditStrategy::Full" in assign_region, "AC4: Full hard face keeps the same gate")
    must(
        "fopen" not in assign_region and "fread" not in assign_region,
        "AC4: no new file I/O in the evidence block",
    )
    for fn in [
        "ac11_wal_prune_fold_4240",
        "ac12_full_scan_exhausted_face_4240",
        "ac13_pruned_reason_4240",
        "ac14_wiring_non_goals_4240",
    ]:
        must(f"static void {fn}()" in tst, f"AC4: {fn} defined in test_evolution_audit_decision_forensic.cpp")
    main_region = _between(tst, "int main()", "if (g_failed)")
    for fn in [
        "ac11_wal_prune_fold_4240",
        "ac12_full_scan_exhausted_face_4240",
        "ac13_pruned_reason_4240",
        "ac14_wiring_non_goals_4240",
    ]:
        must(f"{fn}();" in main_region, f"AC4: {fn} registered in main")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4240.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4240.cpp (per #81934)",
    )
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4240-*")),
        "AC4: no docs/design/4240-* (per #1655)",
    )
    must("check_wal_prune_fold_4240.py" in build, "AC4: build.py registers the linter")
    must("check_wal_prune_fold_4240.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\ncheck_wal_prune_fold_4240: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_wal_prune_fold_4240: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
