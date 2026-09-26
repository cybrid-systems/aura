#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4113: resolve_stamped Stage-1 consults ref.tenant_id only, so any
# residual wash site that sets ref.tenant_id = caller while the #3415
# occupancy ring still holds a foreign owner false-allows Stage-1
# (same-tenant) with poisoned blame. The P0 twin #4112 closed the Agent
# export/handoff choke (finalize_agent_export consults before the stamp
# wash); this belt re-adjudicates at the resolve face itself,
# dual-track with restamp_read_ref (#3772).
#
# AC1 - the belt sits inside resolve_stamped, after Stage 1 (isolation)
#       and before Stage 2 (get_safe) — ordering is the contract: the
#       belt re-adjudicates the caller-stamped ref BEFORE any slot touch.
# AC2 - the belt mirrors the family consult: Strict | (Restricted+MT)
#       regime gate, exact-stamp ladder (existing_stamp_for_node), hygiene
#       borrow (last_hygiene), collision borrow (occupying_stamp_for_node),
#       deny only for non-zero foreign occupancy, routed through the shared
#       check_workspace_isolation face with required = 0 and the VERDICT
#       HONORED (allowed cross-grant target passes), deterministic
#       "isolation-deny: ref-tenant=" reason + nullopt before Stage 2.
# AC3 - no second model: exactly one #4113 belt citation in the security
#       TU; the belt reuses the existing consult atomics and the prior
#       contracts stay intact (finalize_agent_export still cites #4112,
#       restamp_read_ref still cites #3772).
# AC4 - no invented surface: no fetch_add inside the belt window,
#       "schema-4113" does not exist, compiler_metrics_fields.inc keeps
#       cross_tenant_provenance_deny_total and gains no 4113 marker (the
#       reused IsolationDeny face carries the signal, no new metric).
# AC5 - runtime ACs live in
#       tests/compiler/test_query_result_full_provenance.cpp (per #81934
#       extend-the-family; dispatcher wired; runner summary extended), no
#       tests/core/test_issue_4113.cpp, no docs/design/4113-* (per #1655).
#
# Self-test:
#   python3 scripts/check_resolve_stamped_occupancy_4113.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

RESOLVE_SIG = "Evaluator::resolve_stamped(const ast::FlatAST::StableNodeRef& ref"
STAGE1 = "Stage 1: isolation"
STAGE2 = "Stage 2: FlatAST validity"


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks (does not need to handle raw strings /
    trigraphs).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    sec_raw = _read("src/compiler/evaluator_security.cpp")
    sec = _strip_cpp_comments(sec_raw)
    test_src = _read("tests/compiler/test_query_result_full_provenance.cpp")

    cite = -1
    stage2 = -1
    stage1 = -1

    # -- AC1: belt sits after Stage 1, before Stage 2 --
    rs = sec_raw.find(RESOLVE_SIG)
    if rs < 0:
        fails.append("AC1: resolve_stamped signature not found")
    else:
        stage1 = sec_raw.find(STAGE1, rs)
        cite = sec_raw.find("Issue #4113", rs)
        stage2 = sec_raw.find(STAGE2, rs)
        if stage1 < 0:
            fails.append("AC1: Stage 1 isolation block not found")
        if cite < 0:
            fails.append("AC1: resolve_stamped does not cite Issue #4113")
        if stage2 < 0:
            fails.append("AC1: Stage 2 get_safe block not found")
        if stage1 >= 0 and cite >= 0 and stage2 >= 0 and not (stage1 < cite < stage2):
            fails.append("AC1: belt must sit after Stage 1 and before Stage 2 (ordering is the contract)")

    # -- AC2: family mirror inside the belt window --
    if cite >= 0 and stage2 >= 0 and cite < stage2:
        window = _strip_cpp_comments(sec_raw[cite:stage2])
        if "strict || (restricted && mt)" not in window:
            fails.append("AC2: consult regime predicate missing (Strict | Restricted+MT)")
        if "existing_stamp_for_node" not in window:
            fails.append("AC2: exact-stamp ladder missing (existing_stamp_for_node)")
        if "last_hygiene" not in window:
            fails.append("AC2: hygiene borrow missing (last_hygiene)")
        if "occupying_stamp_for_node" not in window:
            fails.append("AC2: collision borrow missing (occupying_stamp_for_node)")
        if "existing != 0 && existing != caller" not in window:
            fails.append("AC2: belt must deny only non-zero foreign occupancy")
        if "if (!check_workspace_isolation(caller, existing" not in window:
            fails.append("AC2: verdict must be honored (allowed cross-grant target passes)")
        if "/*required=*/0" not in sec_raw[cite:stage2]:
            fails.append("AC2: belt deny must pass required = 0 (tenant boundary is the contract)")
        if "isolation-deny: ref-tenant=" not in window:
            fails.append("AC2: deny reason must name the owner tenant (isolation-deny: ref-tenant=)")
        if "return std::nullopt;" not in window:
            fails.append("AC2: deny must return nullopt before the Stage 2 slot touch")

    # -- AC3: single belt, no second model, prior contracts intact --
    # (comment citations live in // text — pin them on the RAW TU text)
    if sec_raw.count("Issue #4113") != 1:
        fails.append("AC3: expected exactly one #4113 belt citation in the security TU")
    if "Issue #4112" not in sec_raw:
        fails.append("AC3: finalize_agent_export must keep citing #4112 (prior contract intact)")
    if "Issue #3772" not in sec_raw:
        fails.append("AC3: restamp_read_ref must keep citing #3772 (prior contract intact)")

    # -- AC4: no invented surface --
    if cite >= 0 and stage2 >= 0 and cite < stage2:
        window = _strip_cpp_comments(sec_raw[cite:stage2])
        if "fetch_add" in window:
            fails.append("AC4: belt window must not bump counters directly (reused face only)")
    if "schema-4113" in sec:
        fails.append("AC4: schema-4113 must not exist")
    mf = _read("src/compiler/compiler_metrics_fields.inc")
    if "cross_tenant_provenance_deny_total" not in mf:
        fails.append("AC4: cross_tenant_provenance_deny_total missing from metrics fields")
    if "4113" in mf:
        fails.append("AC4: no 4113 marker allowed in metrics fields (no new metric)")

    # -- AC5: runtime ACs hosted + no invent --
    if "AC4113/AC1" not in test_src:
        fails.append("AC5: runtime ACs must live in test_query_result_full_provenance.cpp")
    if "test_ac4113_1_washed_ref_foreign_occupancy_denied();" not in test_src:
        fails.append("AC5: dispatcher must call test_ac4113_1_washed_ref_foreign_occupancy_denied")
    if "#4113 AC tests PASSED" not in test_src:
        fails.append("AC5: runner summary must include #4113")
    if (ROOT / "tests/core/test_issue_4113.cpp").exists():
        fails.append("AC5: tests/core/test_issue_4113.cpp must not exist (per #81934)")
    if list((ROOT / "docs" / "design").glob("4113-*")):
        fails.append("AC5: docs/design/4113-* must not exist (per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("PASS check_resolve_stamped_occupancy_4113: all ACs green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
