#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4135: stamp grant EffectProvenance.node_id on NodeId-gated grant
# faces. Forensic join for grants was strong on mid + tenant + fiber +
# Mutation epoch, but every grant mint path passed node_id=0 into
# make_grant_provenance, so blame/replay could not answer which workspace
# node authorized a Mutate/MSE/session grant without correlating a separate
# mutate audit row. The NodeId-gate allow path
# (Evaluator::require_effect_for_node_id, the #2942/#3040 mandate face) now
# notes the gated node on a boundary TLS that shares the
# clear_boundary_audit_mid lifecycle, and all six grant mint faces read it:
# grant_capability / grant_effect_capability / grant_effect_durable /
# grant_effect_durable_sticky / grant_effect_session (evaluator_security.cpp)
# plus the MacroSelfEvo policy seed (evaluator_primitives_security.cpp).
# EffectProvenance.node_id flows to CapabilityGrant.bound_node_id through
# the existing registry grant paths (no schema change, no new query key, no
# new mid-metrics counter). Paths without a node context keep 0 (honest
# unset; the Soft mid-invent arm does NOT spill into the node axis).
#
# AC1 - all six grant mint faces read the boundary node TLS and the literal
#       /*node_id=*/0 mint shape is gone from both mint TUs.
# AC2 - the NodeId-gate allow path notes the target node
#       (require_effect_for_node_id, after the #3724 stamp anchor); deny
#       paths keep the TLS unset.
# AC3 - typed_mutation_audit.h declares g_tls_boundary_target_node with
#       note/read helpers and clear_boundary_audit_mid() clears it (same
#       lifecycle as the #3016/#3971 boundary mid/tenant TLS).
# AC4 - the ACs extend the existing test family: ac4135_1..5 in
#       tests/core/test_capability_single_use_consume.cpp wired into the
#       dispatched run_test_inert_session_mid_3723 runner, and
#       ac22_grant_node_join_after_wrap_4135 in
#       tests/core/test_audit_replay_join.cpp wired into
#       run_test_audit_replay_join (wrap keeps SE/WAL replay + node join).
# AC5 - no tests/**/test_issue_4135.cpp (per #81934), no docs/design/4135-*
#       (per #1655), and no compulsory node invent (the node TLS has no
#       fallback synthesis; grants never force a non-zero node).
#
# Self-test:
#   python3 scripts/check_grant_node_provenance_4135.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> int:
    fails: list[str] = []

    # -- AC1: all six mint faces read the boundary node TLS --
    sec = _read("src/compiler/evaluator_security.cpp")
    node_read = "const auto node = typed_audit::current_boundary_target_node();"
    mint_call = "make_grant_provenance(mid, force_bind, node, fiber)"
    if sec.count(node_read) != 5:
        fails.append(f"AC1: expected 5 node TLS reads, found {sec.count(node_read)}")
    if sec.count(mint_call) != 5:
        fails.append(f"AC1: expected 5 mint faces on the node local, found {sec.count(mint_call)}")
    if "/*node_id=*/0, fiber" in sec:
        fails.append("AC1: literal node_id=0 mint shape remains in evaluator_security.cpp")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    if prim.count(node_read) != 1:
        fails.append("AC1: MSE policy seed face does not read the node TLS exactly once")
    if prim.count(mint_call) != 1:
        fails.append("AC1: MSE seed face does not mint on the node local exactly once")
    if "/*node=*/0, fiber" in prim:
        fails.append("AC1: literal node=0 MSE seed shape remains")

    # -- AC2: the NodeId-gate allow path notes the target node --
    note_call = "typed_audit::note_boundary_target_node(static_cast<std::uint32_t>(node_id));"
    if sec.count(note_call) != 1:
        fails.append("AC2: gate-allow note call missing or duplicated")
    else:
        gate_fn = sec.find("bool Evaluator::require_effect_for_node_id(")
        note_pos = sec.find(note_call)
        stamp_anchor = sec.find("ref = make_stamped_ref(node_id); // #3724 occupancy after allow")
        if gate_fn == -1 or stamp_anchor == -1 or not (gate_fn < stamp_anchor < note_pos):
            fails.append("AC2: note call is not inside require_effect_for_node_id after the allow stamp")
    if "note_boundary_target_node" not in _read("src/compiler/typed_mutation_audit.h"):
        fails.append("AC2: note helper missing from typed_mutation_audit.h")

    # -- AC3: TLS + lifecycle in typed_mutation_audit.h --
    audit = _read("src/compiler/typed_mutation_audit.h")
    for needle in (
        "inline thread_local std::uint32_t g_tls_boundary_target_node = 0;",
        "[[nodiscard]] inline std::uint32_t current_boundary_target_node() noexcept",
        "inline void note_boundary_target_node(std::uint32_t node) noexcept",
    ):
        if needle not in audit:
            fails.append(f"AC3: missing {needle}")
    clear_fn = audit.find("inline void clear_boundary_audit_mid() noexcept")
    if clear_fn == -1:
        fails.append("AC3: clear_boundary_audit_mid() missing")
    else:
        clear_win = audit[clear_fn : clear_fn + 300]
        if "g_tls_boundary_target_node = 0;" not in clear_win:
            fails.append("AC3: clear_boundary_audit_mid() does not clear the node TLS")

    # -- AC4: ACs extend the existing test family --
    cap_test = _read("tests/core/test_capability_single_use_consume.cpp")
    for fn in (
        "ac4135_1_session_grant_node_under_gate_boundary",
        "ac4135_2_grant_without_node_context_stamps_zero",
        "ac4135_3_mse_seed_stamps_gate_node",
        "ac4135_4_soft_off_no_node_invent",
        "ac4135_5_source_cite",
    ):
        if fn not in cap_test:
            fails.append(f"AC4: test_capability_single_use_consume.cpp missing {fn}")
    if cap_test.count("ac4135_1_session_grant_node_under_gate_boundary();") != 1:
        fails.append("AC4: ac4135_1 not wired exactly once into the dispatched runner")
    if cap_test.count("ac4135_5_source_cite();") != 1:
        fails.append("AC4: ac4135_5 not wired exactly once into the dispatched runner")
    rj_test = _read("tests/core/test_audit_replay_join.cpp")
    if "ac22_grant_node_join_after_wrap_4135" not in rj_test:
        fails.append("AC4: test_audit_replay_join.cpp missing ac22_grant_node_join_after_wrap_4135")
    if rj_test.count("ac22_grant_node_join_after_wrap_4135();") != 1:
        fails.append("AC4: ac22 not wired exactly once into run_test_audit_replay_join")

    # -- AC5: no new test file / design doc / node invent --
    if list(ROOT.glob("tests/**/test_issue_4135.cpp")):
        fails.append("AC5: tests/**/test_issue_4135.cpp must not exist (per #81934)")
    if list(ROOT.glob("docs/design/4135-*")):
        fails.append("AC5: docs/design/4135-* must not exist (per #1655)")
    if "? 1" in audit[audit.find("g_tls_boundary_target_node") : audit.find("g_tls_boundary_target_node") + 600]:
        fails.append("AC5: node TLS carries a fallback synthesis (compulsory node invent)")
    for invented in ("grant_node_invent", "node_forced_stamp", "grant_node_id_total"):
        if invented in audit or invented in sec:
            fails.append(f"AC5: invented node-axis metric/key detected ({invented})")

    if fails:
        print("check_grant_node_provenance_4135: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_grant_node_provenance_4135: OK (all ACs pass)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
