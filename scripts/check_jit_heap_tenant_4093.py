#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4093: JIT cells and hashes live in process-global tables
# (g_cell_heap / g_hash_tables) that carry no owner principal. A same-tenant
# Mutate grant (the #4018/#3720 dispatch choke passes for the caller's own
# principal) let tenant B overwrite or read tenant A's cell / hash entry,
# and the unchecked JIT pair field read returned any id's car/cdr.
#
# Fix shape (reuse the #4057 pair-slot pattern — no second capability model):
# parallel g_cell_tenants / g_hash_tenants stamped at alloc from
# aura_jit_owner_capability_tenant() (explicit-tenant alloc seams for the
# light-link shadow case, per #4036); the production face (sandbox != 0 and
# Strict or Restricted+MT) refuses a foreign / unstamped slot with the
# access skipped and the deny fired through the owner's
# check_workspace_isolation (IsolationDeny SE, fiber id + Mutation epoch);
# Mutate stays the write choke; pair_field_locked consults
# g_pair_slot_tenants the same way. Soft/Off: face probe precedes any
# tenant-array load (zero-cost contract — no tenant load).
#
# AC1 — g_cell_tenants is a file-local parallel to the static g_cell_heap;
#       aura_new_cell stamps the owning principal inside the publish
#       critical section (after the push_back, before the unlock) from the
#       existing owner hook; the explicit-tenant seam aura_new_cell_tenant
#       (#4036 aura_alloc_closure_tenant shape) drives the same field.
# AC2 — aura_cell_get_checked arms the production-face probe BEFORE loading
#       the stamp (Soft/Off never reads the array) and on deny skips the
#       access (unlock + return the OOB default 0) without dereferencing
#       the slot; the production aura_cell_get wrapper delegates with the
#       owner-hook caller/face values (one impl, two entries).
# AC3 — aura_cell_set keeps the #4018 Mutate require_effect as the write
#       choke (production wrapper) and delegates to aura_cell_set_checked,
#       which gates before the write (write inside the gated branch; deny
#       unlocks and returns).
# AC4 — g_hash_tenants is declared in runtime_shared.h, defined in
#       runtime_ssot.cpp next to g_hash_tables (same SSOT link-order
#       class), stamped at every g_hash_tables.push_back alloc site
#       (the `hash` prim + the three agent-runtime sites) at the publish
#       point from aura_jit_owner_capability_tenant(), and stamped by the
#       aura_hash_alloc_tenant C-ABI alloc seam (#4036 explicit-tenant
#       form — light-link binaries cannot read the strong hook).
# AC5 — aura_hash_ref_checked arms the face probe before the table scan
#       (deny → not-found sentinel, access skipped) with the production
#       aura_hash_ref wrapper delegating hook values; aura_hash_set keeps
#       require_effect first in its wrapper and delegates to
#       aura_hash_set_checked which gates before any table mutation;
#       aura_hash_remove gates inline before the tombstone write.
# AC6 — pair_field_locked consults g_pair_slot_tenants via the same
#       armed/gate pair (face probe before the stamp load) with explicit
#       caller/face; aura_pair_car_unchecked_checked is the seam and the
#       production wrapper passes hook values; set-car!/set-cdr! keep
#       their existing #4057 compare untouched.
# AC7 — service.ixx defines the strong owner hooks
#       (aura_jit_owner_sandbox_mode → owner->effect_sandbox_mode();
#       aura_jit_owner_check_isolation → owner->check_workspace_isolation(),
#       unwired owner → face off / allow) and
#       aura_jit_prim_dispatch_stub.cpp carries the weak fail-safe stubs.
# AC8 — the runtime resets clear g_cell_tenants / g_hash_tenants
#       alongside their arrays (a stale stamp must never re-attach to a
#       reused index); aura_alloc_pair_tenant is the pair alloc-stamp seam;
#       the behavioral ACs extend
#       tests/compiler/test_dispatch_required_effects.cpp (per #81934 —
#       no tests/**/test_issue_4093.cpp) and no docs/design/4093-* exists
#       (per #1655).
#
# Self-test:
#   python3 scripts/check_jit_heap_tenant_4093.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    rt = _strip_cpp_comments(_read("src/compiler/aura_jit_runtime.cpp"))

    # ── AC1: cell stamps — parallel array + owner-hook stamp at publish ──
    if "static std::vector<std::uint64_t> g_cell_tenants;" not in rt:
        fails.append("AC1: g_cell_tenants parallel array missing")
    a = rt.find("int64_t aura_new_cell()")
    if a == -1:
        fails.append("AC1: aura_new_cell not found")
    else:
        win = rt[a : a + 800]
        pub = win.find("g_cell_heap.push_back(0)")
        stamp = win.find("g_cell_tenants[static_cast<std::size_t>(id)] = aura_jit_owner_capability_tenant()")
        unl = win.find("aura_unlock_workspace_write()")
        if pub == -1 or stamp == -1:
            fails.append("AC1: aura_new_cell does not stamp the owner tenant at publish")
        else:
            if stamp < pub:
                fails.append("AC1: aura_new_cell stamps before the slot publish")
            if unl != -1 and stamp > unl:
                fails.append("AC1: aura_new_cell stamps after the unlock (race window)")
    seam = rt.find('extern "C" int64_t aura_new_cell_tenant(int64_t owner_tenant)')
    if seam == -1:
        fails.append("AC1: aura_new_cell_tenant explicit-tenant seam missing")
    else:
        win = rt[seam : seam + 700]
        if "g_cell_tenants[static_cast<std::size_t>(id)]" not in win:
            fails.append("AC1: aura_new_cell_tenant does not drive the same g_cell_tenants field")

    # ── AC2: cell read — checked impl gates; production wrapper delegates ──
    a = rt.find("aura_cell_get_checked(int64_t cell_id, std::uint64_t caller_tenant")
    if a == -1:
        fails.append("AC2: aura_cell_get_checked seam not found")
    else:
        win = rt[a : a + 1600]
        armed = win.find("jit_tenant_gate_armed(sandbox_mode)")
        load = win.find("g_cell_tenants[")
        gate = win.find('jit_tenant_gate(slot_tenant, caller_tenant, "cell-ref")')
        if armed == -1 or gate == -1:
            fails.append("AC2: cell read seam lacks the armed/gate pair")
        if load != -1 and armed != -1 and load < armed:
            fails.append("AC2: cell read loads the stamp before the face probe (Soft tenant load)")
        if gate != -1:
            deny_ret = win.find("return 0;", gate)
            read = win.find("result = g_cell_heap[", gate)
            if deny_ret == -1 or read == -1 or deny_ret > read:
                fails.append("AC2: cell read deny path does not skip the slot read")
    a = rt.find("int64_t aura_cell_get(int64_t cell_id)")
    if a == -1:
        fails.append("AC2: aura_cell_get wrapper not found")
    else:
        win = rt[a : a + 300]
        if "aura_cell_get_checked(cell_id, aura_jit_owner_capability_tenant()" not in win or (
            "aura_jit_owner_sandbox_mode()" not in win
        ):
            fails.append("AC2: aura_cell_get wrapper does not delegate with owner-hook values")

    # ── AC3: cell write — Mutate choke in the wrapper, gate in the seam ──
    a = rt.find("void aura_cell_set(int64_t cell_id, int64_t val)")
    if a == -1:
        fails.append("AC3: aura_cell_set wrapper not found")
    else:
        win = rt[a : a + 700]
        if "aura_jit_owner_require_effect" not in win:
            fails.append("AC3: aura_cell_set lost the #4018 Mutate choke")
        if "aura_cell_set_checked(cell_id, val," not in win:
            fails.append("AC3: aura_cell_set does not delegate to the checked seam")
    a = rt.find("aura_cell_set_checked(int64_t cell_id, int64_t val,")
    if a == -1:
        fails.append("AC3: aura_cell_set_checked seam not found")
    else:
        win = rt[a : a + 1600]
        armed = win.find("jit_tenant_gate_armed(sandbox_mode)")
        wr = win.find("g_cell_heap[static_cast<size_t>(cell_id)] = val")
        gate = win.find('jit_tenant_gate(slot_tenant, caller_tenant, "cell-set!")')
        if armed == -1 or gate == -1:
            fails.append("AC3: cell write seam lacks the armed/gate pair")
        if wr != -1 and gate != -1 and gate > wr:
            fails.append("AC3: cell write gate does not precede the write")

    # ── AC4: g_hash_tenants SSOT + stamps at every push_back site + seam ──
    sh = _read("src/compiler/runtime_shared.h")
    if "extern std::vector<std::uint64_t> g_hash_tenants;" not in sh:
        fails.append("AC4: runtime_shared.h does not declare g_hash_tenants")
    if 'extern "C" std::uint64_t aura_jit_owner_capability_tenant(void) noexcept' not in sh:
        fails.append("AC4: runtime_shared.h does not expose the owner hook decl for stamp sites")
    ssot = _read("src/compiler/runtime_ssot.cpp")
    if "std::vector<std::uint64_t> g_hash_tenants;" not in ssot:
        fails.append("AC4: runtime_ssot.cpp does not define g_hash_tenants")
    expected_sites = {
        "src/compiler/evaluator_primitives_vector.cpp": 1,
        "src/compiler/evaluator_primitives_agent.cpp": 3,
    }
    stamp_line = "g_hash_tenants[hidx] = aura_jit_owner_capability_tenant()"
    for rel, want in expected_sites.items():
        src = _strip_cpp_comments(_read(rel))
        got = src.count(stamp_line)
        if got != want:
            fails.append(f"AC4: {rel} expected {want} hash-alloc stamp(s), found {got}")
        idx = 0
        for _ in range(got):
            idx = src.find(stamp_line, idx)
            pub = src.rfind("g_hash_tables.push_back(ht);", 0, idx)
            ret = src.find("return make_hash(hidx);", idx)
            if pub == -1 or ret == -1 or idx < pub or idx > ret:
                fails.append(f"AC4: {rel} stamp not between the table publish and make_hash")
            idx += len(stamp_line)
    a = rt.find('extern "C" int64_t aura_hash_alloc_tenant(int64_t owner_tenant)')
    if a == -1:
        fails.append("AC4: aura_hash_alloc_tenant C-ABI alloc seam missing")
    else:
        win = rt[a : a + 900]
        pub = win.find("g_hash_tables.push_back(ht);")
        stamp = win.find("g_hash_tenants[hidx] = static_cast<std::uint64_t>(owner_tenant);")
        if pub == -1 or stamp == -1 or stamp < pub:
            fails.append("AC4: aura_hash_alloc_tenant does not stamp at the table publish")

    # ── AC5: hash gates — checked impls gate; wrappers keep the choke ──
    a = rt.find("aura_hash_ref_checked(int64_t hash_val, int64_t key_val,")
    if a == -1:
        fails.append("AC5: aura_hash_ref_checked seam not found")
    else:
        win = rt[a : a + 1800]
        armed = win.find("jit_tenant_gate_armed(sandbox_mode)")
        load = win.find("g_hash_tenants[hidx]")
        scan = win.find("auto* ht = g_hash_tables[hidx];")
        gate = win.find('jit_tenant_gate(slot_tenant, caller_tenant, "hash-ref")')
        if armed == -1 or gate == -1:
            fails.append("AC5: hash read seam lacks the armed/gate pair")
        if load != -1 and armed != -1 and load < armed:
            fails.append("AC5: hash read loads the stamp before the face probe")
        if scan == -1 or armed == -1 or armed > scan:
            fails.append("AC5: hash read gate does not precede the table scan")
        deny_ret = win.find("return result;", armed if armed != -1 else 0)
        if deny_ret == -1 or (scan != -1 and deny_ret > scan):
            fails.append("AC5: hash read deny path does not return the sentinel before the scan")
    a = rt.find("int64_t aura_hash_ref(int64_t hash_val, int64_t key_val)")
    if a == -1:
        fails.append("AC5: aura_hash_ref wrapper not found")
    else:
        win = rt[a : a + 300]
        if "aura_hash_ref_checked(hash_val, key_val, aura_jit_owner_capability_tenant()" not in win:
            fails.append("AC5: aura_hash_ref wrapper does not delegate with owner-hook values")
    a = rt.find("int64_t aura_hash_set(int64_t hash_val, int64_t pair_val)")
    if a == -1:
        fails.append("AC5: aura_hash_set wrapper not found")
    else:
        win = rt[a : a + 600]
        if "aura_jit_owner_require_effect" not in win:
            fails.append("AC5: aura_hash_set lost the #3720 Mutate choke")
        if "aura_hash_set_checked(hash_val, pair_val," not in win:
            fails.append("AC5: aura_hash_set does not delegate to the checked seam")
    a = rt.find("aura_hash_set_checked(int64_t hash_val, int64_t pair_val,")
    if a == -1:
        fails.append("AC5: aura_hash_set_checked seam not found")
    else:
        win = rt[a : a + 1400]
        armed = win.find("jit_tenant_gate_armed(sandbox_mode)")
        gate = win.find('jit_tenant_gate(slot_tenant, caller_tenant, "hash-set!")')
        if armed == -1 or gate == -1:
            fails.append("AC5: hash write seam lacks the armed/gate pair")
    a = rt.find("int64_t aura_hash_remove(int64_t hash_val, int64_t key_val)")
    if a == -1:
        fails.append("AC5: aura_hash_remove not found")
    else:
        win = rt[a : a + 1400]
        armed = win.find("jit_tenant_gate_armed(aura_jit_owner_sandbox_mode())")
        gate = win.find('jit_tenant_gate(slot_tenant, aura_jit_owner_capability_tenant(), "hash-remove!")')
        scan = win.find("auto* ht = g_hash_tables[hidx];")
        if armed == -1 or gate == -1:
            fails.append("AC5: aura_hash_remove lacks the inline armed/gate pair")
        if scan == -1 or armed == -1 or armed > scan:
            fails.append("AC5: aura_hash_remove gate does not precede the table scan")

    # ── AC6: pair_field_locked consults the stamp; set-car!/set-cdr! intact ──
    a = rt.find("static int64_t pair_field_locked")
    if a == -1:
        fails.append("AC6: pair_field_locked not found")
    else:
        win = rt[a : a + 1600]
        armed = win.find("jit_tenant_gate_armed(sandbox_mode)")
        load = win.find("g_pair_slot_tenants")
        if armed == -1 or load == -1:
            fails.append("AC6: pair_field_locked lacks the armed/stamp consult")
        if load != -1 and armed != -1 and load < armed:
            fails.append("AC6: pair_field_locked loads the stamp before the face probe")
    if "aura_pair_car_unchecked_checked(" not in rt:
        fails.append("AC6: aura_pair_car_unchecked_checked seam missing")
    a = rt.find("int64_t aura_pair_car_unchecked(int64_t pair_val)")
    if a == -1:
        fails.append("AC6: aura_pair_car_unchecked wrapper not found")
    else:
        # Whitespace-tolerant: clang-format may rewrap the call arguments.
        norm = " ".join(rt[a : a + 900].split())
        if '"pair-car", aura_jit_owner_capability_tenant(), aura_jit_owner_sandbox_mode())' not in norm:
            fails.append("AC6: pair wrapper does not pass owner-hook values to the gate")
    pair = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_pair.cpp"))
    if pair.count("slot_tenant != ev.capability_tenant_id()") < 2:
        fails.append("AC6: set-car!/set-cdr! #4057 compare was disturbed")

    # ── AC7: strong hooks in service.ixx + weak stubs ──
    svc = _read("src/compiler/service.ixx")
    a = svc.find('extern "C" int aura_jit_owner_sandbox_mode(void) noexcept')
    if a == -1:
        fails.append("AC7: service.ixx lacks the strong sandbox hook")
    else:
        win = svc[a : a + 400]
        if "owner->effect_sandbox_mode()" not in win:
            fails.append("AC7: sandbox hook does not route to the owner evaluator")
    a = svc.find('extern "C" int aura_jit_owner_check_isolation(')
    if a == -1:
        fails.append("AC7: service.ixx lacks the strong isolation hook")
    else:
        win = svc[a : a + 700]
        if "owner->check_workspace_isolation(" not in win:
            fails.append("AC7: isolation hook does not route to check_workspace_isolation")
    stub = _read("src/compiler/aura_jit_prim_dispatch_stub.cpp")
    for hook in ("aura_jit_owner_sandbox_mode", "aura_jit_owner_check_isolation"):
        if stub.find(hook) == -1:
            fails.append(f"AC7: weak stub for {hook} missing")

    # ── AC8: reset parity + pair seam + test/doc placement ──
    if "g_cell_tenants.clear();" not in rt:
        fails.append("AC8: cell reset does not clear g_cell_tenants")
    if "g_hash_tenants.clear();" not in rt:
        fails.append("AC8: hash reset does not clear g_hash_tenants")
    a = rt.find('extern "C" int64_t aura_alloc_pair_tenant(int64_t car, int64_t cdr, int64_t owner_tenant)')
    if a == -1:
        fails.append("AC8: aura_alloc_pair_tenant pair alloc-stamp seam missing")
    else:
        win = rt[a : a + 800]
        if "g_pair_slot_tenants[static_cast<std::size_t>(id)]" not in win:
            fails.append("AC8: aura_alloc_pair_tenant does not drive the same stamp field")
    if "aura_cell_get_checked" not in sh:
        fails.append("AC8: runtime_shared.h does not declare the checked seams")
    tst = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if "ac4093_1_foreign_deny" not in tst or "ac4093_source_cite" not in tst:
        fails.append("AC8: behavioral ACs missing from test_dispatch_required_effects.cpp")
    if (ROOT / "tests/core/test_issue_4093.cpp").exists():
        fails.append("AC8: tests/**/test_issue_4093.cpp must not exist (per #81934)")
    if any((ROOT / "docs" / "design").glob("4093-*")):
        fails.append("AC8: docs/design/4093-* must not exist (per #1655)")

    if fails:
        print(f"check_jit_heap_tenant_4093: FAIL ({len(fails)})")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_jit_heap_tenant_4093: PASS (8 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
