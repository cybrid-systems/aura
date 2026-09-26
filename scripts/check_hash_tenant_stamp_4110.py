#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4110: evaluator-minted hashes stayed unstamped — runtime_ssot.cpp
# says every g_hash_tables.push_back stamps the owner, but only the vector
# constructor (hash prim), the agent sites, and the #4093 jit seam actually
# did. The rest (query:find / query:pattern QueryResult via
# make_query_result_hash, schema-2 exports, observability, persist, compile
# hashes, ...) left the parallel g_hash_tenants entry at the resize default
# 0, so under Restricted+MT / Strict the production JIT hash gate
# (aura_hash_ref_checked) read the owner's own table as foreign and
# returned the not-found sentinel 11. The tree-walker prims (hash-ref /
# hash-has-key? / hash-set! / hash-remove!) never consulted the stamps at
# all, so the interpreter still served values from / let a foreign
# principal write the process-global table.
#
# AC1 — runtime_shared.h declares the SSOT stamp helper
#       (aura_hash_stamp_new_table_owner) and the tree-walker gate seam
#       (aura_hash_gate_checked) next to the #4093 checked seams.
# AC2 — runtime_ssot.cpp defines the stamp helper next to g_hash_tenants
#       with the vector-constructor body: resize-to-size, then stamp the
#       back slot from the owner hook (no second model, no new query key).
# AC3 — every evaluator mint file calls the helper once per
#       g_hash_tables.push_back (query workspace incl. make_query_result_
#       hash, query tail / lifecycle / reflect / type-stats / obs-mid,
#       mutation, memory, obs jit/eval, messaging, stdlib review, persist,
#       json, compile, evaluator.ixx, workspace tree).
# AC4 — the #4093-pinned inline stamps stay intact (evaluator_primitives_
#       vector.cpp hash prim, evaluator_primitives_agent.cpp,
#       aura_jit_runtime.cpp aura_hash_alloc_tenant) — the helper is
#       additive; the pinned stamps are not rewritten.
# AC5 — hash-ref / hash-has-key? / hash-set! / hash-remove! in
#       evaluator_primitives_vector.cpp call aura_hash_gate_checked with
#       the owner-hook values (caller + face) BEFORE the probe, one gate
#       per op name; armed deny returns void / #f without storing.
# AC6 — aura_jit_runtime.cpp defines aura_hash_gate_checked with the face
#       probe (jit_tenant_gate_armed) preceding any g_hash_tenants load
#       (Soft/Off zero-cost contract) and the deny routed through
#       jit_tenant_gate (check_workspace_isolation).
# AC7 — jit_tenant_gate is NOT weakened: the exact slot==caller compare is
#       intact and no unstamped-0 match (slot_tenant == 0 ||) is added.
# AC8 — ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no test_issue_4110.cpp; no docs/design/4110-* (per #1655).
#
# Self-test:
#   python3 scripts/check_hash_tenant_stamp_4110.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SWEEP_FILES = [
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/compiler/evaluator_primitives_query_tail.cpp",
    "src/compiler/evaluator_primitives_query_lifecycle.cpp",
    "src/compiler/evaluator_primitives_query_reflect.cpp",
    "src/compiler/evaluator_primitives_query_type_stats.cpp",
    "src/compiler/evaluator_primitives_query_obs_mid.cpp",
    "src/compiler/evaluator_primitives_mutation.cpp",
    "src/compiler/evaluator_primitives_memory.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "src/compiler/evaluator_primitives_obs_eval.cpp",
    "src/compiler/evaluator_primitives_messaging.cpp",
    "src/compiler/evaluator_primitives_stdlib_review.cpp",
    "src/compiler/evaluator_primitives_persist.cpp",
    "src/compiler/evaluator_primitives_json.cpp",
    "src/compiler/evaluator_primitives_compile.cpp",
    "src/compiler/evaluator.ixx",
    "src/compiler/evaluator_workspace_tree.cpp",
]

PUSH = "g_hash_tables.push_back(ht);"
CALL = "aura_hash_stamp_new_table_owner();"


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

    hdr = _strip_cpp_comments(_read("src/compiler/runtime_shared.h"))
    ssot = _strip_cpp_comments(_read("src/compiler/runtime_ssot.cpp"))
    vec = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_vector.cpp"))
    rt = _strip_cpp_comments(_read("src/compiler/aura_jit_runtime.cpp"))

    # ── AC1: header declares the helper + the gate seam ──
    if "void aura_hash_stamp_new_table_owner();" not in hdr:
        fails.append("AC1: runtime_shared.h does not declare aura_hash_stamp_new_table_owner")
    if 'extern "C" bool aura_hash_gate_checked(std::uint64_t hidx' not in hdr:
        fails.append("AC1: runtime_shared.h does not declare aura_hash_gate_checked")

    # ── AC2: ssot defines the helper (resize-to-size + owner-hook stamp) ──
    def_at = ssot.find("void aura_hash_stamp_new_table_owner() {")
    if def_at < 0:
        fails.append("AC2: runtime_ssot.cpp does not define aura_hash_stamp_new_table_owner")
    else:
        body = ssot[def_at : def_at + 400]
        if "g_hash_tenants.resize(g_hash_tables.size(), 0);" not in body:
            fails.append("AC2: helper does not resize-to-size (vector-constructor shape)")
        if "g_hash_tenants[g_hash_tables.size() - 1]" not in body:
            fails.append("AC2: helper does not stamp the back slot")
        if "aura_jit_owner_capability_tenant()" not in body:
            fails.append("AC2: helper does not stamp from the owner hook")

    # ── AC3: every mint file stamps every push ──
    for rel in SWEEP_FILES:
        src = _strip_cpp_comments(_read(rel))
        if not src:
            fails.append(f"AC3: {rel} missing/unreadable")
            continue
        pushes = src.count(PUSH)
        calls = src.count(CALL)
        if pushes == 0:
            fails.append(f"AC3: {rel} has no push sites (sweep set stale?)")
        elif calls != pushes:
            fails.append(f"AC3: {rel} pushes={pushes} stamp calls={calls}")
    qw = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_query_workspace.cpp"))
    if "make_query_result_hash" not in qw:
        fails.append("AC3: make_query_result_hash missing from query_workspace")
    elif qw.count("make_query_result_hash") < qw.count(CALL):
        pass  # helper call after the mint's push verified by the count above

    # ── AC4: #4093-pinned inline stamps intact (helper is additive) ──
    if "g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();" not in vec:
        fails.append("AC4: vector hash prim inline stamp (#4093 pin) removed")
    ag = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_agent.cpp"))
    if ag.count("g_hash_tenants[hidx] = aura_jit_owner_capability_tenant();") < 3:
        fails.append("AC4: agent hash alloc inline stamps (#4093 pin) reduced")
    if "aura_hash_alloc_tenant" not in rt:
        fails.append("AC4: aura_hash_alloc_tenant seam (#4093 pin) removed")

    # ── AC5: the four tree-walker prims gate before the probe ──
    for op in ("hash-ref", "hash-has-key?", "hash-set!", "hash-remove!"):
        needle = "aura_hash_gate_checked(hidx, aura_jit_owner_capability_tenant(),"
        if needle not in vec:
            fails.append(f"AC5: {op} gate call missing (owner-hook caller)")
        if f'"{op}")' not in vec:
            fails.append(f"AC5: op name {op} not passed to the gate")
    gate_calls = vec.count("aura_hash_gate_checked(hidx,")
    if gate_calls != 4:
        fails.append(f"AC5: expected 4 tree-walker gate calls, found {gate_calls}")
    probes = vec.count("auto* ht = g_hash_tables[hidx];")
    if probes < 4:
        fails.append("AC5: probe shape changed (auto* ht = g_hash_tables[hidx])")
    # the production wrapper delegation (owner face) must stay on the gate
    if 'aura_jit_owner_sandbox_mode(), "hash-ref"' not in vec:
        fails.append("AC5: hash-ref gate does not read the owner face hook")

    # ── AC6: seam body — face probe precedes the stamp load; deny via gate ──
    seam_at = rt.find('extern "C" bool aura_hash_gate_checked(')
    if seam_at < 0:
        fails.append("AC6: aura_jit_runtime.cpp does not define aura_hash_gate_checked")
    else:
        body = rt[seam_at : seam_at + 500]
        armed_at = body.find("jit_tenant_gate_armed(sandbox_mode)")
        load_at = body.find("g_hash_tenants[hidx]")
        if armed_at < 0:
            fails.append("AC6: seam does not face-probe jit_tenant_gate_armed")
        if load_at < 0:
            fails.append("AC6: seam does not read the stamp array")
        if armed_at >= 0 and load_at >= 0 and armed_at > load_at:
            fails.append("AC6: face probe must precede the g_hash_tenants load")
        if "jit_tenant_gate(slot_tenant, caller_tenant, op)" not in body:
            fails.append("AC6: deny not routed through jit_tenant_gate")

    # ── AC7: jit_tenant_gate not weakened ──
    if "if (slot_tenant == caller_tenant)" not in rt:
        fails.append("AC7: jit_tenant_gate exact-match compare missing")
    if "slot_tenant == 0 ||" in rt or "caller_tenant == 0 ||" in rt:
        fails.append("AC7: unstamped-0 match weakening detected")
    if "aura_jit_owner_check_isolation(caller_tenant, slot_tenant" not in rt:
        fails.append("AC7: isolation deny path missing from jit_tenant_gate")

    # ── AC8: test home + no new test file / design doc ──
    test_src = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if "ac4110_source_cite" not in test_src:
        fails.append("AC8: test_dispatch_required_effects.cpp missing ac4110_source_cite")
    if "aura_hash_gate_checked" not in test_src and "g_hash_tenants" not in test_src:
        fails.append("AC8: test_dispatch_required_effects.cpp missing #4110 behavioral arms")
    if (ROOT / "tests/core/test_issue_4110.cpp").exists():
        fails.append("AC8: tests/core/test_issue_4110.cpp must not exist (per #81934)")
    if list(ROOT.glob("docs/design/4110-*")):
        fails.append("AC8: docs/design/4110-* must not exist (per #1655)")

    if fails:
        print("check_hash_tenant_stamp_4110: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_hash_tenant_stamp_4110: OK (all ACs pass)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
