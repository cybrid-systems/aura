#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4057: set-car!/set-cdr! wrote the process-level g_pair_slots without
# any ownership check — the index space is process-shared, so a same-tenant
# Mutate grant (the #4036 dispatch choke passes for the caller's own
# principal) still let an Agent write another tenant's JIT pair handed over
# via mailbox (idx >= pairs_.size() falls through to the process slot, which
# carried no tenant field).
#
# AC1 — aura_alloc_pair / aura_alloc_pair_arena stamp the owning principal
#       into g_pair_slot_tenants (parallel array, resize-guarded) via the
#       existing aura_jit_owner_capability_tenant hook, at the same publish
#       point as the slot (#4036 closure-table shape; no second model).
# AC2 — set-car!/set-cdr! g_pair_slots branches compare the slot stamp
#       against the caller principal BEFORE the write under the production
#       face (sandbox mode != 0 and Strict or Restricted+MT); the deny goes
#       through the existing check_workspace_isolation / record_audit path
#       (fiber id + Mutation epoch) and the bodies stay body-choke-free
#       (no second require_effect — single-use grants must not double-consume).
# AC3 — the local pairs_ branch precedes the gated branch and stays
#       unconditional (idx < pairs.size() writes the caller's own pair with
#       no tenant gate).
# AC4 — the tenant read is guarded by the production-face condition
#       (prod_pair_face && slot_tenant != caller): Soft/Off never reads the
#       tenant array and the else-branch write keeps today's behavior.
# AC5 — runtime_shared.h declares g_pair_slot_tenants; runtime_ssot.cpp
#       defines it next to g_pair_slots (same SSOT link-order class).
# AC6 — PairSlotCleanup clears g_pair_slot_tenants alongside g_pair_slots
#       (a stale stamp must never survive a reset and re-attach to a reused
#       index).
# AC7 — ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no test_issue_4057.cpp; no docs/design/4057-* (per #1655).
#
# Self-test:
#   python3 scripts/check_pair_slot_tenant_4057.py
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

    # ── AC1: alloc stamps the owning principal (parallel array) ──
    if "g_pair_slot_tenants" not in rt:
        fails.append("AC1: g_pair_slot_tenants missing from aura_jit_runtime.cpp")
    stamps = rt.count("g_pair_slot_tenants[static_cast<std::size_t>(id)] = aura_jit_owner_capability_tenant()")
    if stamps != 2:
        fails.append(f"AC1: expected aura_alloc_pair + aura_alloc_pair_arena stamp sites, found {stamps}")
    # Stamp sits inside the publish critical section (after the push_back,
    # before unlock) so the stamp is never visible without the slot.
    for anchor in (
        "int64_t aura_alloc_pair(int64_t car, int64_t cdr)",
        "int64_t aura_alloc_pair_arena(int64_t car, int64_t cdr)",
    ):
        a = rt.find(anchor)
        if a == -1:
            fails.append(f"AC1: {anchor} not found")
            continue
        win = rt[a : a + 1600]
        if "g_pair_slots.push_back(slot)" not in win:
            fails.append(f"AC1: {anchor} does not publish the slot")
        if "g_pair_slot_tenants" not in win:
            fails.append(f"AC1: {anchor} does not stamp the owner tenant")
        if win.find("g_pair_slot_tenants") < win.find("g_pair_slots.push_back(slot)"):
            fails.append(f"AC1: {anchor} stamps before the slot publish")
        if "aura_unlock_workspace_write()" in win and win.find("g_pair_slot_tenants") > win.find(
            "aura_unlock_workspace_write()"
        ):
            fails.append(f"AC1: {anchor} stamps after the unlock (race window)")

    # ── AC2: set-car!/set-cdr! gate before the process-slot write ──
    pair = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_pair.cpp"))
    for nm in ("set-car!", "set-cdr!"):
        anchor = f'add, ev, "{nm}"'
        a = pair.find(anchor)
        if a == -1:
            fails.append(f"AC2: {nm} registration not found")
            continue
        win = pair[a : a + 2600]
        if "require_effect" in win:
            fails.append(f"AC2: {nm} body adds a second effect choke (double-consume)")
        if "#4057" not in win:
            fails.append(f"AC2: {nm} body lacks the #4057 citation")
        if "prod_pair_face" not in win:
            fails.append(f"AC2: {nm} body has no production-face condition")
        if "g_pair_slot_tenants" not in win:
            fails.append(f"AC2: {nm} body never reads the slot tenant array")
        if "slot_tenant != ev.capability_tenant_id()" not in win:
            fails.append(f"AC2: {nm} body does not compare the caller principal")
        if "check_workspace_isolation" not in win:
            fails.append(f"AC2: {nm} deny does not go through check_workspace_isolation")
        # Gate sits before the write; the write stays inside the gated branch.
        gate = win.find("slot_tenant != ev.capability_tenant_id()")
        wr = win.find(f"g_pair_slots[idx]->{'car' if nm == 'set-car!' else 'cdr'} = a[1].val")
        if gate == -1 or wr == -1 or gate > wr:
            fails.append(f"AC2: {nm} gate does not precede the process-slot write")

    # Face arms: sandbox mode non-zero AND (Strict mode 2 / process strict /
    # multi-tenant env). Single-tenant Restricted and Soft/Off stay permissive.
    if pair.count("ev.effect_sandbox_mode() != 0") < 2:
        fails.append("AC2: production face does not require sandbox mode != 0")
    if pair.count("::aura::core::sandbox::is_strict()") < 2:
        fails.append("AC2: production face does not consult is_strict()")
    if pair.count("::aura::core::provenance::multi_tenant_env_active()") < 2:
        fails.append("AC2: production face does not consult multi_tenant_env_active()")

    # ── AC3: local pairs_ branch stays first and unconditional ──
    for nm, field in (("set-car!", "car"), ("set-cdr!", "cdr")):
        a = pair.find(f'add, ev, "{nm}"')
        if a == -1:
            continue
        win = pair[a : a + 2600]
        local = win.find("idx < pairs.size()")
        gated = win.find("prod_pair_face")
        if local == -1 or gated == -1 or local > gated:
            fails.append(f"AC3: {nm} local pairs_ branch is not the first arm")
        if f"pairs[idx].{field} = a[1]" not in win:
            fails.append(f"AC3: {nm} local pairs_ write regressed")

    # ── AC4: Soft/Off never reads the tenant array ──
    # The only tenant reads are inside the gated branch: the read must be
    # textually after the face conjunction (mode != 0 && (...)) and the deny
    # condition must be the conjunction with prod_pair_face.
    if pair.count("prod_pair_face && slot_tenant != ev.capability_tenant_id()") != 2:
        fails.append("AC4: deny condition is not gated on the production face")
    read_guard = pair.count("(idx < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[idx] : 0")
    if read_guard != 2:
        fails.append("AC4: tenant read lacks the index guard (short array = unstamped)")

    # ── AC5: declaration + SSOT definition ──
    rsh = _read("src/compiler/runtime_shared.h")
    if "extern std::vector<std::uint64_t> g_pair_slot_tenants;" not in rsh:
        fails.append("AC5: runtime_shared.h does not declare g_pair_slot_tenants")
    if "#4057" not in rsh:
        fails.append("AC5: runtime_shared.h declaration lacks the #4057 citation")
    ssot = _strip_cpp_comments(_read("src/compiler/runtime_ssot.cpp"))
    if "std::vector<std::uint64_t> g_pair_slot_tenants;" not in ssot:
        fails.append("AC5: runtime_ssot.cpp does not define g_pair_slot_tenants")
    gpos = ssot.find("std::vector<PairSlot*> g_pair_slots;")
    tpos = ssot.find("std::vector<std::uint64_t> g_pair_slot_tenants;")
    if gpos == -1 or tpos == -1 or tpos < gpos:
        fails.append("AC5: tenant array is not defined beside g_pair_slots in the SSOT TU")

    # ── AC6: reset clears the stamps with the slots ──
    clear = rt.find("g_pair_slots.clear();")
    if clear == -1:
        fails.append("AC6: PairSlotCleanup g_pair_slots.clear() missing")
    else:
        win = rt[max(0, clear - 200) : clear + 200]
        if "g_pair_slot_tenants.clear();" not in win:
            fails.append("AC6: g_pair_slot_tenants.clear() missing at the reset site")

    # ── AC7: ACs extend the dispatch family; no new files ──
    test_src = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if test_src.count("#4057") < 5:
        fails.append("AC7: test_dispatch_required_effects.cpp lacks the #4057 AC block")
    for stray in (
        (ROOT / "tests" / "core").glob("test_issue_4057*.cpp"),
        (ROOT / "tests" / "compiler").glob("test_issue_4057*.cpp"),
        (ROOT / "tests" / "issues").glob("test_issue_4057*.cpp"),
    ):
        if any(stray):
            fails.append("AC7: test_issue_4057.cpp exists (must extend the dispatch family, #81934)")
    if any((ROOT / "docs" / "design").glob("4057-*")):
        fails.append("AC7: docs/design/4057-* exists (banned per #1655)")

    if fails:
        print("check_pair_slot_tenant_4057: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_pair_slot_tenant_4057: OK (AC1-AC7)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
