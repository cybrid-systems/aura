#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4039: two residual entries let a bare NodeId pass as the caller.
# (1) require_effect_for_node_id: the #3641 same-slot collision borrow
#     assigned the slot occupant's tenant to `existing`; when that tenant
#     EQUALED the caller the function fell through to the caller-stamp
#     branch, check_boundary_ex saw ref.tenant == caller, and a same-tenant
#     write on a node the caller never owned was authorized (A's node 1
#     holds slot 1; B's live 257 cannot take the slot; A requires 257).
# (2) gate_compile_node_effect: arg.tenant==0 && principal==0 returned true
#     before any isolation consult, so a fresh Restricted Evaluator could
#     write compile metadata through a bare id with no require_effect.
# Fix pins: the collision borrow is tracked and denied through the shared
# unstamped-ref face (check_boundary_ex ref_tenant==0 under the consult
# face) BEFORE any caller-stamp; foreign occupants keep the #3641 on_ref
# deny; exact stamps and true misses keep the #2056/#3415 caller-stamp
# allow; the compile gate's principal-0 early allow is deleted (Soft/Off
# short-circuit and the arg.tenant!=0 on_ref route stay).
#
# AC1 — collision tracked + denied: require_effect_for_node_id declares
#       collision_borrow, sets it when occ.node_id != queried node_id, and
#       denies (check_workspace_isolation(caller, ref_tenant=0, ...) +
#       nodeid_only_entry_prevented_total + return false) for the
#       same-tenant/untenanted occupant BEFORE the caller-stamp fall-through.
# AC2 — foreign face intact: the #3641 borrow (existing = occ.tenant_id)
#       still feeds the existing != caller on_ref deny; no new query key,
#       no second occupancy store (kNodeOccupancyRingSlots untouched).
# AC3 — gate early allow deleted: the compile TU no longer contains the
#       `arg.tenant == 0 && ev.capability_tenant_id() == 0` allow.
# AC4 — gate shape preserved: the Soft/Off short-circuit stays the first
#       gate exit, and the bare-NodeId fall-through routes into
#       require_effect_for_node_id (principal 0 included).
# AC5 — host test wired: ac4039_1..6 exist in
#       tests/compiler/test_require_effect_auto_isolation.cpp and
#       run_test_nodeid_collision_4039 is dispatched by
#       test_security_capability_batch.cpp; no tests/**/test_issue_4039.cpp;
#       no docs/design/4039-* (per #1655/#81934).
#
# Self-test:
#   python3 scripts/check_nodeid_collision_4039.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SECURITY = ROOT / "src" / "compiler" / "evaluator_security.cpp"
COMPILE = ROOT / "src" / "compiler" / "evaluator_primitives_compile.cpp"
PROV = ROOT / "src" / "core" / "provenance_tracker.hh"
HOST_TEST = ROOT / "tests" / "compiler" / "test_require_effect_auto_isolation.cpp"
DRIVER = ROOT / "tests" / "compiler" / "test_security_capability_batch.cpp"

errors: list[str] = []


def must(cond: bool, msg: str) -> None:
    if not cond:
        errors.append(msg)


def main() -> int:
    sec = SECURITY.read_text()
    comp = COMPILE.read_text()
    host = HOST_TEST.read_text()
    driver = DRIVER.read_text()

    # AC1 — collision tracked + denied before any caller-stamp.
    fn_pos = sec.find("bool Evaluator::require_effect_for_node_id(")
    must(fn_pos >= 0, "AC1: require_effect_for_node_id not found")
    fn_end = sec.find("Issue #2706", fn_pos)  # next comment block after the body
    fn = sec[fn_pos : fn_end if fn_end > 0 else fn_pos + 9000]
    must("collision_borrow" in fn, "AC1: collision_borrow not tracked")
    must(
        "occ.node_id != 0 && occ.node_id != static_cast<std::uint32_t>(node_id)" in fn,
        "AC1: collision condition (occupant node differs) not present",
    )
    deny_pos = fn.find("collision_borrow && (existing == 0 || existing == caller)")
    must(deny_pos >= 0, "AC1: same-tenant collision deny branch missing")
    if deny_pos >= 0:
        deny = fn[deny_pos : deny_pos + 900]
        must(
            "check_workspace_isolation(caller, /*ref_tenant=*/0" in deny,
            "AC1: deny must route through the shared unstamped-ref face",
        )
        must(
            "nodeid_only_entry_prevented_total" in deny,
            "AC1: deny must bump nodeid_only_entry_prevented_total",
        )
        must("return false;" in deny, "AC1: deny must return false (zero write)")
    must("Issue #4039" in fn, "AC1: fix must cite the issue")

    # AC2 — foreign #3641 face intact; no second store / query key.
    must(
        "existing = occ.tenant_id;" in fn,
        "AC2: #3641 foreign borrow must stay (foreign on_ref deny face)",
    )
    must(
        "existing != 0 && existing != caller" in fn,
        "AC2: foreign occupancy on_ref deny condition must stay",
    )
    must(
        "kNodeOccupancyRingSlots = 256" in PROV.read_text(),
        "AC2: the 256-slot occupancy ring stays (no second store)",
    )

    # AC3 — principal-0 gate early allow deleted.
    must(
        "arg.tenant == 0 && ev.capability_tenant_id() == 0" not in comp,
        "AC3: gate_compile_node_effect principal-0 early allow must be deleted",
    )
    must("Issue #4039" in comp, "AC3: gate fix must cite the issue")

    # AC4 — gate shape preserved.
    gate_pos = comp.find("bool gate_compile_node_effect(")
    must(gate_pos >= 0, "AC4: gate_compile_node_effect not found")
    gate = comp[gate_pos : gate_pos + 2500]
    must(
        "ev.sandbox_mode() == 0 && ev.effect_sandbox_mode() == 0" in gate,
        "AC4: Soft/Off short-circuit must stay",
    )
    must(
        "return true;" in gate,
        "AC4: Soft/Off early return true must stay",
    )
    must(
        "ev.require_effect_for_node_id(bits, op, arg.id)" in gate,
        "AC4: bare NodeId fall-through must route into require_effect_for_node_id",
    )
    must(
        "ev.require_effect_on_ref(bits, op, ref)" in gate,
        "AC4: stamped foreign tenant must keep the on_ref route",
    )

    # AC5 — host test wired + no invent.
    for fnname in (
        "ac4039_1_same_tenant_collision_denies",
        "ac4039_2_exact_stamp_allows",
        "ac4039_3_true_miss_caller_stamp_allows",
        "ac4039_4_principal_zero_gate_denies",
        "ac4039_5_soft_off_gate_unchanged",
        "ac4039_6_source_cite_and_no_invent",
    ):
        must(fnname in host, f"AC5: {fnname} missing in host test")
    must(
        "int run_test_nodeid_collision_4039()" in host,
        "AC5: run_test_nodeid_collision_4039 entry point missing",
    )
    must(
        "run_test_nodeid_collision_4039()" in driver,
        "AC5: batch driver must dispatch run_test_nodeid_collision_4039",
    )
    for p in (
        ROOT / "tests" / "compiler" / "test_issue_4039.cpp",
        ROOT / "tests" / "core" / "test_issue_4039.cpp",
        ROOT / "tests" / "issues" / "test_issue_4039.cpp",
    ):
        must(not p.exists(), f"AC5: {p.name} must not exist (per #81934)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for p in docs.iterdir():
            must("4039-" not in p.name, f"AC5: no docs/design/{p.name} (per #1655)")

    if errors:
        for e in errors:
            print(f"FAIL check_nodeid_collision_4039: {e}")
        return 1
    print("OK check_nodeid_collision_4039: 5/5 ACs pinned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
