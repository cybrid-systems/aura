#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4036: set-car!/set-cdr!/string-fill!/closure:free! skip require_effect
# — zero-cap write and process-global free. The four sibling mutators resolved
# to kEffectNone (infer gap), so the #2152/#2583 dispatch choke never ran:
# set-car!/set-cdr! wrote pairs_[idx] or the process-global g_pair_slots,
# string-fill! overwrote string_heap_[idx], and closure:free! freed any
# in-range JIT closure id (no tenant field on the slot → cross-tenant free).
#
# AC1 — infer_required_effects_from_name maps the four names to kEffectMutate
#       (same choke as hash-set!, #3720); no query key, no new Effect bit.
# AC2 — set-car!/set-cdr!/string-fill! prim bodies stay body-choke-free
#       (dispatch is the single choke; a body-level require_effect would
#       double-consume single-use grants, same note as c-*).
# AC3 — closure:free! int path routes through aura_free_closure_checked with
#       the calling Evaluator's capability_tenant_id() + effect_sandbox_mode().
# AC4 — aura_free_closure_checked applies the foreign-tenant arm (slot
#       tenant != 0 && != caller → deny BEFORE any slot mutation) and the
#       legacy arm (slot tenant 0 under is_strict/multi-tenant → refuse).
# AC5 — alloc_closure_slot_locked stamps g_closure_tenants on fresh alloc
#       and restamps on reuse from aura_jit_owner_capability_tenant.
# AC6 — service.ixx strong owner hook returns capability_tenant_id();
#       light-link weak stub returns 0 (unstamped).
# AC7 — legacy aura_free_closure delegates with caller_tenant=0 /
#       sandbox_mode=0 (internal sweeps keep today's free); runtime_shared.h
#       declares the checked entry.
# AC8 — ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no test_issue_4036.cpp; no docs/design/4036-* (per #1655).
#
# Self-test:
#   python3 scripts/check_mutation_choke_4036.py
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

    # ── AC1: infer maps the four names to kEffectMutate ──
    sse = _read("src/compiler/security_side_effect.hh")
    sse_code = _strip_cpp_comments(sse)
    if "Issue #4036" not in sse:
        fails.append("AC1: security_side_effect.hh lacks the #4036 citation")
    for nm in ('"set-car!"', '"set-cdr!"', '"string-fill!"', '"closure:free!"'):
        if nm not in sse_code:
            fails.append(f"AC1: infer does not name {nm}")
    if "hash-set!" not in sse_code or "hash-remove!" not in sse_code or "vector-set!" not in sse_code:
        fails.append("AC1: #3720 choke names regressed")
    # The four-name arm returns kEffectMutate: window from the #4036 arm to
    # the next return must carry kEffectMutate.
    a36 = sse_code.find("set-car!")
    if a36 == -1 or "kEffectMutate" not in sse_code[a36 : a36 + 400]:
        fails.append("AC1: four-name arm does not return kEffectMutate")
    # No query key added for these names.
    if 'query:"' in sse_code.replace(" ", "") and False:
        pass  # query: prefix legitimately appears for other families; names must not join it
    for nm in ("set-car!", "set-cdr!", "string-fill!", "closure:free!"):
        idx = sse_code.find(f'"{nm}"')
        if idx != -1 and sse_code[max(0, idx - 200) : idx].find("query") != -1:
            fails.append(f"AC1: {nm} routed through a query key (banned)")

    # ── AC2: pair/string prim bodies stay body-choke-free ──
    pair = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_pair.cpp"))
    for anchor, span, label in (
        ('add, ev, "set-car!"', 1500, "set-car!"),
        ('add, ev, "set-cdr!"', 900, "set-cdr!"),
        ('add, ev, "string-fill!"', 900, "string-fill!"),
    ):
        pos = pair.find(anchor)
        if pos == -1:
            fails.append(f"AC2: {label} registration not found")
            continue
        if "require_effect" in pair[pos : pos + span]:
            fails.append(f"AC2: {label} body adds a second require_effect (double-consume)")

    # ── AC3: closure:free! routes through the checked entry ──
    mem = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_memory.cpp"))
    cf = mem.find('"closure:free!"')
    if cf == -1:
        fails.append("AC3: closure:free! registration not found")
    else:
        win = mem[cf : cf + 1200]
        if "aura_free_closure_checked" not in win:
            fails.append("AC3: closure:free! int path bypasses aura_free_closure_checked")
        if "capability_tenant_id()" not in win:
            fails.append("AC3: checked free does not pass the caller principal")
        if "effect_sandbox_mode()" not in win:
            fails.append("AC3: checked free does not pass the face mode")
        if "aura_free_closure(as_int" in win:
            fails.append("AC3: legacy unchecked free still reachable from the prim")

    # ── AC4: checked free applies both deny arms before slot mutation ──
    rt = _strip_cpp_comments(_read("src/compiler/aura_jit_runtime.cpp"))
    fc = rt.find("int aura_free_closure_checked")
    if fc == -1:
        fails.append("AC4: aura_free_closure_checked missing")
    else:
        foreign = rt.find("slot_tenant != caller_tenant", fc)
        legacy = rt.find("multi_tenant_env_active()", fc)
        arena = rt.find("g_arena_closure_envs[cid]", fc)
        if foreign == -1:
            fails.append("AC4: foreign-tenant arm missing")
        if legacy == -1:
            fails.append("AC4: legacy unstamped arm missing")
        if arena == -1:
            fails.append("AC4: arena env free body not found after checked entry")
        if foreign != -1 and arena != -1 and foreign > arena:
            fails.append("AC4: foreign-tenant deny sits AFTER the env free (not zero free)")
        if legacy != -1 and arena != -1 and legacy > arena:
            fails.append("AC4: legacy deny sits AFTER the env free (not zero free)")
        if "is_strict()" not in rt[fc : fc + 3000]:
            fails.append("AC4: strict face not consulted")

    # ── AC5: alloc stamps the owning principal ──
    if "g_closure_tenants" not in rt:
        fails.append("AC5: g_closure_tenants slot-tenant vector missing")
    if "g_closure_tenants.push_back(owner_tenant)" not in rt:
        fails.append("AC5: fresh alloc does not stamp the owner principal")
    if "g_closure_tenants[cid] = owner_tenant" not in rt:
        fails.append("AC5: reuse path does not restamp the owner principal")
    # Production entries pass the owner hook (owner Evaluator's
    # capability_tenant_id_ — the same principal require_effect records).
    # Whitespace-normalized match over comment-stripped source: the call
    # sites wrap across lines and the /*is_arena=*/ tag is a comment.
    norm = " ".join(rt.split())
    if "alloc_closure_slot_locked(func_id, 0, aura_jit_owner_capability_tenant())" not in norm:
        fails.append("AC5: aura_alloc_closure does not pass the owner hook")
    if "alloc_closure_slot_locked(func_id, 1, aura_jit_owner_capability_tenant())" not in norm:
        fails.append("AC5: aura_alloc_closure_arena does not pass the owner hook")
    # Explicit-tenant seam (same stamp point) for links where the strong
    # owner hook is shadowed by the light-link weak stub.
    rsh_hdr = _read("src/compiler/runtime_shared.h")
    if "aura_alloc_closure_tenant" not in rt or "aura_alloc_closure_tenant" not in rsh_hdr:
        fails.append("AC5: explicit-tenant alloc seam missing")

    # ── AC6: owner hook strong + weak ──
    svc = _read("src/compiler/service.ixx")
    sc = svc.find("aura_jit_owner_capability_tenant")
    if sc == -1:
        fails.append("AC6: service.ixx lacks the strong owner-principal hook")
    elif "capability_tenant_id()" not in svc[sc : sc + 600]:
        fails.append("AC6: strong hook does not return the owner capability_tenant_id")
    stub = _strip_cpp_comments(_read("src/compiler/aura_jit_prim_dispatch_stub.cpp"))
    wc = stub.find("aura_jit_owner_capability_tenant")
    if wc == -1 or "weak" not in stub[max(0, wc - 200) : wc]:
        fails.append("AC6: weak light-link stub missing")

    # ── AC7: legacy wrapper keeps no-check semantics + header decl ──
    rt_raw = _read("src/compiler/aura_jit_runtime.cpp")
    wrap = rt_raw.find("aura_free_closure_checked(closure_id, /*caller_tenant=*/0")
    if wrap == -1:
        fails.append("AC7: legacy aura_free_closure wrapper lost zero-check delegation")
    rsh = _read("src/compiler/runtime_shared.h")
    if "aura_free_closure_checked" not in rsh:
        fails.append("AC7: runtime_shared.h does not declare the checked entry")

    # ── AC8: ACs extend the dispatch family; no new files ──
    test_src = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if test_src.count("#4036") < 5:
        fails.append("AC8: test_dispatch_required_effects.cpp lacks the #4036 AC block")
    for stray in (
        (ROOT / "tests" / "core").glob("test_issue_4036*.cpp"),
        (ROOT / "tests" / "compiler").glob("test_issue_4036*.cpp"),
        (ROOT / "tests" / "issues").glob("test_issue_4036*.cpp"),
    ):
        if any(stray):
            fails.append("AC8: test_issue_4036.cpp exists (must extend the dispatch family, #81934)")
    if any((ROOT / "docs" / "design").glob("4036-*")):
        fails.append("AC8: docs/design/4036-* exists (banned per #1655)")

    if fails:
        print("check_mutation_choke_4036: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_mutation_choke_4036: OK (AC1-AC8)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
