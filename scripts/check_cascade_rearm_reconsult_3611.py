#!/usr/bin/env python3
# scripts/check_cascade_rearm_reconsult_3611.py -- Issue #3611 source-cite gate.
#
# AC1: per-name peel walk is index-based with a value copy of `name` so the
#      #3168 attribution block can push newly-armed edge endpoints into
#      dirty_names mid-loop (range-for would never see them; a reference
#      binding would dangle across reallocation).
# AC2: attributed new edges pull BOTH endpoints into the peel set
#      (dirty_names.push_back(peer) if missing) via the existing
#      mark_caller_body_dirty (#3474 shape) + finish_cascade_soa_dirty_sync_.
# AC3: want_partial is reconsulted after the peer marks from the post-mark
#      dirty_n + impact_ub (should_partial_relower_impact_checked_prod,
#      #3310 production gate); reconsult fail-closed takes full via the
#      existing partial_forced_full_by_impact_total distinguisher.
# AC4: attribution distinguisher cascade_rearm_new_edge_only_total retained;
#      block stays gated behind the armed!=0 re-arm window (Soft / Off +
#      clean = zero extra); no new mutex, no new query key, no new metrics
#      field.
# AC5: ac3611 rows live in test_cascade_decision_residual_atomic.cpp; no
#      docs/design/*3611*, no tests/**/test_issue_3611.cpp.
# AC6: build.py wires this linter.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SVC = "src/compiler/service.ixx"
TEST = "tests/compiler/test_cascade_decision_residual_atomic.cpp"
BUILD = "build.py"
QUERY = "src/compiler/evaluator_primitives_obs_eval.cpp"
OBS = "src/compiler/observability_metrics.h"

IDX_ANCHOR = "Issue #3611: index-based walk"
ATTR_ANCHOR = "Issue #3611: attribution used to keep the stale"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(svc: str, test: str, build: str, query: str, obs: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1: index-based peel walk + value copy.
    must(IDX_ANCHOR, "AC1 cite", svc)
    must("for (std::size_t dn_i = 0; dn_i < dirty_names.size(); ++dn_i) {", "AC1 loop", svc)
    must("const std::string name = dirty_names[dn_i];", "AC1 value copy", svc)

    # AC2: peer endpoints enter the peel set via the existing helper shape.
    must(ATTR_ANCHOR, "AC2 cite", svc)
    must("dirty_names.push_back(peer)", "AC2 peer push", svc)
    must("pit->second.mark_caller_body_dirty()", "AC2 peer mark", svc)
    must("finish_cascade_soa_dirty_sync_(pit->second)", "AC2 peer SoA sync", svc)

    # AC3: reconsult from post-mark dirty_n + impact_ub; fail-closed full.
    pos = svc.find(ATTR_ANCHOR)
    if pos < 0:
        fails.append("AC3: attribution cite missing (window unreachable)")
    else:
        win = svc[pos : pos + 3200]
        must("impact_upper_bound_for_entry_", "AC3 impact_ub", win)
        must("should_partial_relower_impact_checked_prod", "AC3 reconsult", win)
        must("partial_forced_full_by_impact_total.fetch_add", "AC3 fail-closed bump", win)
        must("mark_all_blocks_dirty", "AC3 fail-closed full", win)
    must(
        "metrics_.cascade_rearm_new_edge_only_total.fetch_add",
        "AC3 attribution distinguisher retained",
        svc,
    )

    # AC4: gated behind the armed!=0 re-arm window; no new observables.
    gate = svc.find("if (rearm_observed_mid_loop && want_partial) {")
    if gate < 0 or pos < 0 or pos < gate:
        fails.append("AC4: #3611 block not gated behind armed!=0 re-arm window")
    must("std::mutex cascade_decision_mtx_", "AC4 #3135 mutex unchanged", svc)
    must_not("schema-3611", "AC4 no new query key", query)
    must_not("3611", "AC4 no new metrics field", obs)

    # AC5: test rows present; no invent.
    must("ac3611_1_peer_enters_peel_set", "AC5 test AC1", test)
    must("ac3611_4_peer_soak_lookup_stays_hot", "AC5 test soak", test)
    must("Issue #3611", "AC5 test cite", test)
    for rel in ("tests/issues/test_issue_3611.cpp", "tests/compiler/test_issue_3611.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: forbidden {rel} exists (#81967)")
    if any(p.name.find("3611-") >= 0 for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC5: forbidden docs/design/*3611-* exists (#1655)")

    # AC6: build.py wiring.
    must("check_cascade_rearm_reconsult_3611", "AC6 build.py wiring", build)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        # Positive sample: every row's anchor present → zero failures.
        sample = (
            "if (rearm_observed_mid_loop && want_partial) {\n"
            + IDX_ANCHOR
            + "\nfor (std::size_t dn_i = 0; dn_i < dirty_names.size(); ++dn_i) {\n"
            "const std::string name = dirty_names[dn_i];\n" + ATTR_ANCHOR + "\ndirty_names.push_back(peer)\n"
            "pit->second.mark_caller_body_dirty()\n"
            "finish_cascade_soa_dirty_sync_(pit->second)\n"
            "impact_upper_bound_for_entry_\n"
            "should_partial_relower_impact_checked_prod\n"
            "partial_forced_full_by_impact_total.fetch_add\n"
            "mark_all_blocks_dirty\n"
            "metrics_.cascade_rearm_new_edge_only_total.fetch_add\n"
            "std::mutex cascade_decision_mtx_\n"
        )
        pad = "x" * 3400
        ok_fails = _rows(
            sample + pad,
            "ac3611_1_peer_enters_peel_set ac3611_4_peer_soak_lookup_stays_hot Issue #3611",
            "check_cascade_rearm_reconsult_3611",
            "clean",
            "clean",
        )
        # Negative sample: empty sources → every must row fires.
        neg_fails = _rows("", "", "", "schema-3611", "3611")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        if len(neg_fails) < 10:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(SVC), _read(TEST), _read(BUILD), _read(QUERY), _read(OBS))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("cascade rearm reconsult (#3611) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
