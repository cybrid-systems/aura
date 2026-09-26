#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4094: JIT closure capture (aura_closure_capture) is the native write
# for closure env cells. It took the closure-table mutex + workspace write
# lock and stored with no aura_jit_owner_require_effect call and no
# g_closure_tenants compare — a tenant with no Mutate grant still got the
# write, and tenant B's JIT could overwrite tenant A's captured cell under
# Restricted+MT or Strict with no IsolationDeny (the slot stamp from
# alloc_closure_slot_locked was never read on the capture path).
#
# Fix shape (reuse the #4036 free model + the #4093 seam shape — no second
# capability model): aura_closure_capture_checked(closure_id, idx, val,
# caller_tenant, sandbox_mode) is the gated write body; the production
# wrapper keeps today's Soft/Off store (face probe on
# aura_jit_owner_sandbox_mode() precedes any choke or stamp read) and adds
# the Mutate choke on the production face only; the tenant arms are free's
# (foreign stamp refuse; unstamped 0 fail-closed under Strict /
# multi-tenant) with the deny routed through the owner's
# check_workspace_isolation (IsolationDeny SE, fiber id + Mutation epoch).
# aura_closure_env_get is the value-observation read for the ACs
# (unisolated diagnostic family of aura_closure_get_env_gen; not a JIT
# surface).
#
# AC1 — no second model: the capture gate reads the existing
#       g_closure_tenants stamp array (stamped at alloc_closure_slot_locked
#       from aura_jit_owner_capability_tenant(), driven by the #4036
#       aura_alloc_closure_tenant seam); no capture-specific tenant vector
#       is added.
# AC2 — aura_closure_capture_checked arms the free arms after the
#       bounds/freed checks and before any env mutation: foreign stamp
#       refuse; unstamped 0 fail-closed under Strict / multi-tenant; the
#       deny routes through aura_jit_owner_check_isolation with the
#       "closure-capture" op and stores nothing (arena + heap store paths
#       are both after the gate).
# AC3 — the production wrapper consults the choke only when the owner face
#       is armed (sandbox_mode != 0): aura_jit_owner_require_effect(Mutate,
#       "closure-capture") == 0 → no store; it delegates to the checked
#       seam with the owner-hook caller/face values (one impl, two
#       entries); a light-link unwired owner (weak stubs → face 0) keeps
#       today's store for every existing caller.
# AC4 — Soft/Off zero-cost: inside the seam, the face probe
#       (sandbox_mode != 0) precedes the g_closure_tenants load.
# AC5 — runtime_shared.h declares aura_closure_capture_checked and
#       aura_closure_env_get; the read is defined in the runtime and is
#       NOT registered as a JIT surface in aura_jit.cpp.
# AC6 — the behavioral ACs extend
#       tests/compiler/test_dispatch_required_effects.cpp (per #81934 —
#       no tests/**/test_issue_4094.cpp) and no docs/design/4094-* exists
#       (per #1655).
#
# Self-test:
#   python3 scripts/check_closure_capture_tenant_4094.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose.
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

    # ── AC1: the gate reads the existing stamp array; no second model ──
    if "static std::vector<std::uint64_t> g_closure_tenants;" not in rt:
        fails.append("AC1: g_closure_tenants stamp array missing")
    if "g_closure_capture_tenants" in rt:
        fails.append("AC1: a capture-specific tenant vector was added (second model)")
    seam_alloc = rt.find('extern "C" int64_t aura_alloc_closure_tenant(')
    if seam_alloc == -1:
        fails.append("AC1: aura_alloc_closure_tenant seam missing (#4036)")
    else:
        win = rt[seam_alloc : seam_alloc + 400]
        if "alloc_closure_slot_locked(" not in win:
            fails.append("AC1: aura_alloc_closure_tenant does not drive the shared stamp point")

    # ── AC2: the checked seam holds free's arms; deny routed; gate first ──
    seam = rt.find("aura_closure_capture_checked(int64_t closure_id, int64_t idx, int64_t val,")
    cap = rt.find("void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val)")
    if seam == -1 or cap == -1 or cap <= seam:
        fails.append("AC2: aura_closure_capture_checked seam not found before the wrapper")
    else:
        win = rt[seam:cap]
        face = win.find("if (sandbox_mode != 0) {")
        stamp = win.find("g_closure_tenants[cid]")
        foreign = win.find("slot_tenant != 0 && slot_tenant != caller_tenant")
        strict = win.find("::aura::core::sandbox::is_strict()")
        mt = win.find("::aura::core::provenance::multi_tenant_env_active()")
        deny = win.find("aura_jit_owner_check_isolation(")
        op = win.find('"closure-capture"')
        is_arena = win.find("const bool is_arena")
        if face == -1:
            fails.append("AC2: seam lacks the production-face gate")
        if stamp == -1:
            fails.append("AC2: seam does not read g_closure_tenants")
        if foreign == -1:
            fails.append("AC2: foreign-stamp refuse arm missing (free's #4036 arm)")
        if strict == -1 or mt == -1:
            fails.append("AC2: unstamped 0 fail-closed arm missing (Strict / MT)")
        if deny == -1 or op == -1:
            fails.append("AC2: deny not routed through check_workspace_isolation")
        if is_arena == -1 or face == -1 or face > is_arena:
            fails.append("AC2: tenant gate does not precede the store paths")
        freed = win.find("g_closure_freed[cid] != 0")
        if freed == -1 or face == -1 or face < freed:
            fails.append("AC2: gate not placed after the bounds/freed checks")

    # ── AC3: wrapper choke on the production face; delegates hook values ──
    if cap == -1:
        fails.append("AC3: aura_closure_capture wrapper not found")
    else:
        win = rt[cap : cap + 900]
        if "aura_jit_owner_sandbox_mode()" not in win:
            fails.append("AC3: wrapper does not read the owner face")
        if "face != 0 &&" not in win:
            fails.append("AC3: choke not scoped to the production face (Soft/Off must keep today's store)")
        if "aura_jit_owner_require_effect(" not in win or '"closure-capture"' not in win:
            fails.append("AC3: Mutate choke (closure-capture op) missing in the wrapper")
        norm = " ".join(win.split())
        if "aura_closure_capture_checked(closure_id, idx, val, aura_jit_owner_capability_tenant(), face)" not in norm:
            fails.append("AC3: wrapper does not delegate to the seam with owner-hook values")

    # ── AC4: Soft/Off zero-cost — face probe precedes the stamp load ──
    if seam != -1 and cap != -1 and cap > seam:
        win = rt[seam:cap]
        face = win.find("if (sandbox_mode != 0) {")
        stamp = win.find("g_closure_tenants[cid]")
        if face == -1 or stamp == -1 or stamp < face:
            fails.append("AC4: stamp load precedes the face probe (Soft tenant read)")

    # ── AC5: header decls + observation read (not a JIT surface) ──
    sh = _read("src/compiler/runtime_shared.h")
    if "aura_closure_capture_checked(" not in sh:
        fails.append("AC5: runtime_shared.h does not declare aura_closure_capture_checked")
    if "aura_closure_env_get(" not in sh:
        fails.append("AC5: runtime_shared.h does not declare aura_closure_env_get")
    if "aura_closure_env_get(" not in rt:
        fails.append("AC5: aura_closure_env_get read missing in the runtime")
    jit = _read("src/compiler/aura_jit.cpp")
    if 'reg("aura_closure_env_get"' in jit:
        fails.append("AC5: aura_closure_env_get must not be a JIT-registered surface")

    # ── AC6: behavioral ACs extend the existing test file; placement ──
    tst = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if "ac4094_1_foreign_deny" not in tst or "ac4094_source_cite" not in tst:
        fails.append("AC6: behavioral ACs missing from test_dispatch_required_effects.cpp")
    if (ROOT / "tests/core/test_issue_4094.cpp").exists():
        fails.append("AC6: tests/**/test_issue_4094.cpp must not exist (per #81934)")
    if any((ROOT / "docs" / "design").glob("4094-*")):
        fails.append("AC6: docs/design/4094-* must not exist (per #1655)")

    if fails:
        print(f"check_closure_capture_tenant_4094: FAIL ({len(fails)})")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_closure_capture_tenant_4094: PASS (6 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
