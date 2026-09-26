#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4112: Agent export/handoff washed ref.tenant_id with the caller
# before any occupancy consult. agent-reply / agent-send packed (id . gen)
# and the orch bare-id shim (aura_orch_agent_send_handoff) all flow through
# handoff_ref -> export_held_ref -> finalize_agent_export, which stamped the
# caller tenant into the handle with no #3415 occupancy consult. A foreign
# principal could pack a node id owned by another tenant, receive a
# caller-stamped export, and resolve_stamped Stage-1 would then false-allow
# it (same-tenant) with poisoned blame. restamp_read_ref (#3772) already
# models the correct consult; the export/handoff track now mirrors it.
#
# AC1 - finalize_agent_export carries the #4112 occupancy consult AFTER the
#       function entry and BEFORE the #3204 stamp wash (ordering is the
#       contract: refuse before any stamp write).
# AC2 - the consult mirrors restamp_read_ref: Strict | (Restricted+MT)
#       regime gate, exact-stamp ladder (existing_stamp_for_node), hygiene
#       borrow (last_hygiene), collision borrow (occupying_stamp_for_node),
#       deny routed through check_workspace_isolation with required_effects
#       = 0 and the "agent-export" op, "isolation-deny: ref-tenant=" reason,
#       and a bare `return {}` (NULL id) so Agent delivery sees nullopt.
# AC3 - the call sites stay on the shared choke point: agent-reply /
#       agent-send twins keep stamp_stable_ref + handoff_ref; the orch shim
#       keeps make_stamped_ref + handoff_ref; export_held_ref still wraps
#       finalize_agent_export (no second model, no bypass).
# AC4 - no invented surface: the consult window adds no counter/query key
#       (no fetch_add / insert_kv inside the window), "schema-4112" does not
#       exist, and handoff_ref keeps bumping the existing
#       stable_ref_handoff_reject_total.
# AC5 - runtime ACs live in
#       tests/compiler/test_query_result_full_provenance.cpp (per #81934
#       extend-the-family; no test_issue_4112.cpp) and no docs/design/4112-*
#       (per #1655).
#
# Self-test:
#   python3 scripts/check_agent_export_occupancy_4112.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FINALIZE_SIG = "Evaluator::finalize_agent_export(ast::FlatAST::StableNodeRef ref)"
WASH_COND = "if (ref.tenant_id == 0 && stable_ref_export_hard_reject()) {"


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
    ag = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_agent.cpp"))
    fm = _strip_cpp_comments(_read("src/compiler/evaluator_fiber_mutation.cpp"))
    test_src = _read("tests/compiler/test_query_result_full_provenance.cpp")

    # -- AC1: consult sits inside finalize_agent_export, before the wash --
    # Anchors resolve on the RAW text (the #4112 cite is a comment); code
    # tokens are pinned on the comment-stripped window.
    fin = sec_raw.find(FINALIZE_SIG)
    if fin < 0:
        fails.append("AC1: finalize_agent_export signature not found")
    else:
        cite = sec_raw.find("Issue #4112", fin)
        if cite < 0:
            fails.append("AC1: finalize_agent_export does not cite Issue #4112")
        wash = sec_raw.find(WASH_COND, fin)
        if wash < 0:
            fails.append("AC1: #3204 stamp wash condition not found after finalize entry")
        if cite >= 0 and wash >= 0 and cite > wash:
            fails.append("AC1: consult must precede the stamp wash (ordering is the contract)")

        # -- AC2: restamp_read_ref mirror inside the consult window --
        if cite >= 0 and wash >= 0 and cite < wash:
            window = _strip_cpp_comments(sec_raw[cite:wash])
            if "strict || (restricted && mt)" not in window:
                fails.append("AC2: consult regime predicate missing (Strict | Restricted+MT)")
            if "existing_stamp_for_node" not in window:
                fails.append("AC2: exact-stamp ladder missing (existing_stamp_for_node)")
            if "last_hygiene" not in window:
                fails.append("AC2: hygiene borrow missing (last_hygiene)")
            if "occupying_stamp_for_node" not in window:
                fails.append("AC2: collision borrow missing (occupying_stamp_for_node)")
            if "check_workspace_isolation" not in window:
                fails.append("AC2: deny must route through the shared check_workspace_isolation face")
            if '"agent-export"' not in window:
                fails.append('AC2: deny op must be the shared "agent-export" string')
            if "isolation-deny: ref-tenant=" not in window:
                fails.append("AC2: deny reason must name the owner tenant (isolation-deny: ref-tenant=)")
            if "return {};" not in window:
                fails.append("AC2: deny must return {} (NULL id) before any stamp write")
            # AC4: no invented counter/query key inside the consult window.
            if "fetch_add" in window or "insert_kv" in window:
                fails.append(
                    "AC4: consult window must not add counters/query keys "
                    "(existing IsolationDeny + handoff-reject counters only)"
                )

    # -- AC3: call sites stay on the shared handoff choke point --
    if ag.count("ev.handoff_ref(std::move(held));") < 2:
        fails.append("AC3: agent-reply / agent-send twin handoff sites reduced")
    if ag.count("ev.stamp_stable_ref(held);") < 2:
        fails.append("AC3: agent-reply / agent-send twin stamp sites reduced")
    shim = fm.find('extern "C" int aura_orch_agent_send_handoff(')
    if shim < 0:
        fails.append("AC3: aura_orch_agent_send_handoff shim missing")
    else:
        shim_body = fm[shim : shim + 700]
        if "ev->make_stamped_ref" not in shim_body or "ev->handoff_ref" not in shim_body:
            fails.append("AC3: orch shim must keep make_stamped_ref + handoff_ref")
    if "auto out = finalize_agent_export(std::move(ref));" not in sec:
        fails.append("AC3: export_held_ref must keep wrapping finalize_agent_export")

    # -- AC4: no invented surface --
    if "schema-4112" in sec_raw:
        fails.append("AC4: schema-4112 must not exist (no new query key)")
    if "m->stable_ref_handoff_reject_total.fetch_add" not in sec:
        fails.append("AC4: handoff_ref must keep bumping stable_ref_handoff_reject_total")

    # -- AC5: test home + no new test file / design doc --
    for fn in (
        "test_ac4112_1_foreign_occupancy_handoff_denied",
        "test_ac4112_2_no_wash_on_foreign_same_tenant_ok",
        "test_ac4112_3_soft_zero_extra_consult",
        "test_ac4112_4_orch_bare_id_refused_counters_reused",
        "test_ac4112_5_source_cite",
    ):
        if fn not in test_src:
            fails.append(f"AC5: test_query_result_full_provenance.cpp missing {fn}")
    if "aura_orch_agent_send_handoff" not in test_src:
        fails.append("AC5: runtime ACs must exercise the orch bare-id shim")
    if (ROOT / "tests/core/test_issue_4112.cpp").exists():
        fails.append("AC5: tests/core/test_issue_4112.cpp must not exist (per #81934)")
    if list(ROOT.glob("docs/design/4112-*")):
        fails.append("AC5: docs/design/4112-* must not exist (per #1655)")

    if fails:
        print("check_agent_export_occupancy_4112: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_agent_export_occupancy_4112: OK (all ACs pass)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
