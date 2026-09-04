#!/usr/bin/env python3
"""Issue #3487: query:*-stable latch ∧ torn hard-rejects restamp-lag.

#3386 ORs aura_runtime_multi_worker_production_latched on
query_stable_hard_reject_torn. Residual: allow_query_stable_ref_export
(and the #3230/#3198 export helpers that call it) still keyed the hard
face only off production_defaults_active / should_hard_reject_soft_sibling.
Ready can latch, a later Soft flip leaves the latch set, and query:*-stable
/ ensure-ref / export_ref could stamp-green a pre-mutate gen.

Contract:
  AC1 Latch==1 + torn → restamp-lag even if production_defaults later false
  AC2 Soft + unlatched + under-budget: latch load is after the quiet return
  AC3 No new public query key; reuse restamp-lag / torn counters
  AC4 Held-cap overflow (#3426) still whole-set fail-close
  AC5 Extend query-stable / #3449 / #3386 tests; no invent
  AC6 Source-cite export helpers + latch; linter AFTER #3230

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

    restamp = _read("src/core/flatast_restamp.hh")
    astx = _read("src/core/ast.ixx")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    asr = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    gate = _read("tests/compiler/test_restamp_budget_hard_gate.cpp")
    qrp = _read("tests/compiler/test_query_result_full_provenance.cpp")
    batch = _read("tests/compiler/test_stable_ref_provenance_batch.cpp")
    build = _read("build.py")

    must("kQueryStableRestampLatchExportIssue = 3487", "AC1 stamp", restamp)
    must("kQueryStableRestampLatchExportIssue", "AC1 ast export", astx)
    must("kRestampLagErrorKind", "AC1 reuse error", restamp)
    must("kRestampLagReasonBudgetExceeded", "AC1 reuse reason", restamp)

    allow_i = sec.find("bool Evaluator::allow_query_stable_ref_export")
    allow = sec[max(0, allow_i - 500) : allow_i + 1800] if allow_i >= 0 else ""
    must("Issue #3487", "AC1 allow cite", allow)
    must("should_hard_reject_soft_sibling()", "AC1 hard sibling", allow)
    must("aura_runtime_multi_worker_production_latched() != 0", "AC1 latch OR", allow)
    quiet = allow.find("!ws->restamp_over_budget_torn()")
    if quiet < 0:
        fails.append("AC2: allow missing quiet torn check")
    else:
        pre = allow[:quiet]
        if "aura_runtime_multi_worker_production_latched" in pre:
            fails.append("AC2: latch load on quiet under-budget path")
        after = allow[quiet:]
        if "aura_runtime_multi_worker_production_latched() != 0" not in after:
            fails.append("AC1: latch OR is not on the already-torn path")

    stamp = sec.find("void Evaluator::stamp_query_stable_ref_export")
    swin = sec[stamp : stamp + 900] if stamp >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 stamp uses allow", swin)

    exp_i = sec.find("Evaluator::export_ref(")
    exp = sec[exp_i : exp_i + 500] if exp_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 export_ref uses allow", exp)
    must("Issue #3487", "AC6 export_ref cite", exp)

    exp_s_i = sec.find("Evaluator::export_ref_safe(")
    exp_s = sec[exp_s_i : exp_s_i + 400] if exp_s_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 export_ref_safe uses allow", exp_s)

    held_i = sec.find("Evaluator::export_held_ref(")
    held = sec[held_i : held_i + 400] if held_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 export_held_ref uses allow", held)

    sr_i = qws.find('add("query:stable-ref"')
    sr = qws[sr_i : sr_i + 2200] if sr_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 query:stable-ref uses allow", sr)
    must("Issue #3487", "AC6 query:stable-ref cite", sr)

    ens_i = qws.find('add("query:ensure-ref"')
    ens = qws[ens_i : ens_i + 2800] if ens_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 query:ensure-ref uses allow", ens)
    must("Issue #3487", "AC6 query:ensure-ref cite", ens)

    asr_i = asr.find('add("query:as-stable-ref"')
    asr_win = asr[asr_i : asr_i + 1600] if asr_i >= 0 else ""
    must("allow_query_stable_ref_export", "AC6 as-stable-ref uses allow", asr_win)
    must("Issue #3487", "AC6 as-stable-ref cite", asr_win)

    must("#3487", "AC6 evaluator.ixx cite", ev)

    must("restamp_hot_cone_held_overflow()", "AC4 overflow consult", allow)
    must("kRestampHotConeHeldOverflowIssue", "AC4 overflow stamp", restamp)
    ov = allow.find("restamp_hot_cone_held_overflow()")
    eager = allow.find("node_eagerly_restamped(id)")
    if ov < 0 or eager < 0 or eager < ov:
        fails.append("AC4: overflow still skips eager-bit allow (whole-set fail-close)")

    must("ac3487_1_latch_torn_restamp_lag", "AC5 AC1 test", t)
    must("ac3487_2_soft_unlatched_under_budget", "AC5 AC2 test", t)
    must("3487 AC1: latch+torn rejects even with defaults false", "AC5 live allow", t)
    must("3487 AC1: stable-ref structured", "AC5 live query", t)
    must("3487", "AC5 #3386 suite", gate)
    must("3487", "AC5 #3449 suite", qrp)
    must("3487", "AC5 provenance batch", batch)

    must("check_query_stable_restamp_latch_export_3487", "AC6 build.py", build)
    prev = build.find("check_query_stable_restamp_lag_hard_reject_3230")
    ours = build.find("check_query_stable_restamp_latch_export_3487")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3230")

    must_not("schema-3487", "AC3 no schema-3487", qws)
    must_not("schema-3487", "AC3 no schema in mutate", asr)
    must_not("g_3487_", "AC3 no g_3487_*", restamp)
    must_not("query:restamp-latch", "AC3 no new query", qws)
    if (ROOT / "tests" / "compiler" / "test_issue_3487.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3487.cpp present")
    if (ROOT / "tests" / "issues" / "test_issue_3487.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3487.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3487-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3487 query_stable_restamp_latch_export:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3487 query_stable_restamp_latch_export: latch OR on torn export; Soft quiet")
    return 0


if __name__ == "__main__":
    sys.exit(main())
