#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4134: fail-closed aura_jit_owner_check_isolation when the owner
# Evaluator is unwired. The strong def (service.ixx) and the light-link
# weak stub (aura_jit_prim_dispatch_stub.cpp) both returned 1 (allow)
# unwired. The #4093/#4094 JIT tenant gates decide the access skip locally
# on stamp mismatch and never consult the hook return — heap access stayed
# skipped — but the IsolationDeny SE / fiber+Mutation-epoch audit record
# was lost whenever the face was armed without an owner, and any caller
# trusting the return for access would false-allow. The hook now denies
# (0) unwired (same shape as the fail-closed siblings
# aura_jit_owner_require_effect / aura_jit_owner_sandbox_mode); SE
# emission is best-effort with the existing tenant-isolation counter as
# the observable (no new mid-metrics key); Soft/Off face 0 keeps the
# zero-cost contract (sandbox_mode stub 0 → gates never arm).
#
# AC1 — the strong def fails closed unwired: the `!owner` branch denies
#       (return 0), no unwired `return 1` remains, the owner-wired routing
#       (owner->check_workspace_isolation) is intact, and the #4134
#       rationale is documented at the hook.
# AC2 — the best-effort SE emission is documented and observable via the
#       EXISTING tenant-isolation counter
#       tenant_boundary_violation_prevented_total (a real code bump, not
#       prose only); no new mid-metrics key is invented.
# AC3 — the weak stub denies (0) and the sandbox_mode / require_effect
#       stubs stay 0 (the light-link gates never arm — the Soft/Off
#       zero-cost contract is intact).
# AC4 — jit_tenant_gate / closure-capture skip semantics are untouched
#       (slot==caller compare, hook call site, no unstamped-0 weakening,
#       face probe precedes the stamp load) and the require_effect sibling
#       stays fail-closed unwired with the JIT mutate chokes preceding the
#       stores (hash-set! / cell-set!).
# AC5 — ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no tests/**/test_issue_4134.cpp; no docs/design/4134-*
#       (per #1655).
#
# Self-test:
#   python3 scripts/check_jit_owner_failclosed_4134.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _strip_cpp_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def main() -> int:
    fails: list[str] = []

    # ── AC1: strong def fails closed unwired; owner routing intact ──
    svc = _read("src/compiler/service.ixx")
    iso = svc.find('extern "C" int aura_jit_owner_check_isolation(')
    if iso == -1:
        fails.append("AC1: service.ixx lacks the strong isolation hook")
    else:
        win = svc[iso : iso + 1000]
        unwired = win.find("if (!owner) {")
        deny = win.find("return 0;")
        if unwired == -1 or deny == -1 or deny < unwired:
            fails.append("AC1: unwired owner does not deny (0) in the strong def")
        if "return 1;" in win:
            fails.append("AC1: an unwired allow (return 1) remains in the strong def")
        if "owner->check_workspace_isolation(" not in win:
            fails.append("AC1: owner-wired routing (check_workspace_isolation) missing")
    if "if (!owner)\n        return 1;" in svc:
        fails.append("AC1: legacy unwired allow (return 1) shape still present")
    if "Issue #4134: fail-closed" not in svc:
        fails.append("AC1: #4134 rationale not documented at the hook")

    # ── AC2: best-effort SE observable via the EXISTING isolation counter ──
    if iso != -1:
        win = svc[iso : iso + 1000]
        if "g_tenant_isolation_metrics().tenant_boundary_violation_prevented_total" not in _strip_cpp_comments(win):
            fails.append(
                "AC2: fail-closed deny does not bump the existing tenant_boundary_violation_prevented_total counter"
            )
    for invented in ("isolation_deny_unwired", "unwired_isolation_deny", "unwired_deny_total"):
        if invented in svc:
            fails.append(f"AC2: new mid-metrics key invented ({invented}) — reuse the existing family")

    # ── AC3: weak stubs — isolation denies, sandbox_mode / require_effect
    #        stay 0 (gates never arm light-link; zero-cost contract) ──
    stub = _read("src/compiler/aura_jit_prim_dispatch_stub.cpp")
    stub_iso = stub.find("aura_jit_owner_check_isolation(std::uint64_t, std::uint64_t")
    if stub_iso == -1:
        fails.append("AC3: weak isolation stub missing")
    else:
        swin = stub[stub_iso : stub_iso + 200]
        if "return 0;" not in swin:
            fails.append("AC3: weak isolation stub does not deny (0)")
        if "return 1;" in swin:
            fails.append("AC3: weak isolation stub still allows (1)")
    stub_mode = stub.find("aura_jit_owner_sandbox_mode(void) noexcept")
    if stub_mode == -1 or "return 0;" not in stub[stub_mode : stub_mode + 200]:
        fails.append("AC3: sandbox_mode stub must stay 0 (face off unwired)")
    stub_req = stub.find("aura_jit_owner_require_effect(std::uint16_t bits,")
    if stub_req == -1 or "return 0;" not in stub[stub_req : stub_req + 300]:
        fails.append("AC3: require_effect stub must stay 0 (deny, no silent write)")

    # ── AC4: skip semantics untouched; require_effect sibling intact ──
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    if "if (slot_tenant == caller_tenant)" not in rt:
        fails.append("AC4: jit_tenant_gate exact-match compare missing")
    if "aura_jit_owner_check_isolation(caller_tenant, slot_tenant" not in rt:
        fails.append("AC4: isolation deny path missing from jit_tenant_gate")
    if "slot_tenant == 0 ||" in rt or "caller_tenant == 0 ||" in rt:
        fails.append("AC4: unstamped-0 match weakening detected")
    seam = rt.find("aura_closure_capture_checked(int64_t closure_id, int64_t idx, int64_t val,")
    if seam == -1:
        fails.append("AC4: closure-capture checked seam missing")
    else:
        cap = rt.find("void aura_closure_capture(int64_t closure_id, int64_t idx, int64_t val)")
        swin = rt[seam : cap if cap != -1 and cap > seam else seam + 4200]
        face = swin.find("if (sandbox_mode != 0) {")
        stamp = swin.find("g_closure_tenants[cid]")
        if face == -1 or stamp == -1 or stamp < face:
            fails.append("AC4: seam face probe must precede the stamp load (Soft zero-cost)")
        if "aura_jit_owner_check_isolation(" not in swin:
            fails.append("AC4: seam deny no longer routed through the isolation hook")
    svc_req = svc.find('extern "C" int aura_jit_owner_require_effect(')
    if svc_req == -1 or "if (!owner)\n        return 0;" not in svc[svc_req : svc_req + 320]:
        fails.append("AC4: require_effect sibling must stay fail-closed unwired")
    choke = rt.find('aura_jit_owner_require_effect(aura::compiler::security::kEffectMutate, "hash-set!") == 0')
    if choke == -1:
        fails.append("AC4: hash-set! Mutate choke missing")
    else:
        store = rt.find("aura_hash_set_checked(hash_val, pair_val", choke)
        if store == -1:
            fails.append("AC4: hash-set! store not found downstream of the choke")
    if 'aura_jit_owner_require_effect(aura::compiler::security::kEffectMutate, "cell-set!") == 0' not in rt:
        fails.append("AC4: cell-set! Mutate choke missing")

    # ── AC5: test home + no new test file / design doc ──
    test_src = _read("tests/compiler/test_dispatch_required_effects.cpp")
    for fn in (
        "ac4134_source_cite",
        "ac4134_1_owner_wired_deny_unchanged",
        "ac4134_2_unwired_hook_denies",
        "ac4134_3_soft_zero_cost",
        "ac4134_4_require_effect_precedes_store",
    ):
        if fn not in test_src:
            fails.append(f"AC5: test_dispatch_required_effects.cpp missing {fn}")
    if list(ROOT.glob("tests/**/test_issue_4134.cpp")):
        fails.append("AC5: tests/**/test_issue_4134.cpp must not exist (per #81934)")
    if list(ROOT.glob("docs/design/4134-*")):
        fails.append("AC5: docs/design/4134-* must not exist (per #1655)")

    if fails:
        print("check_jit_owner_failclosed_4134: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_jit_owner_failclosed_4134: OK (all ACs pass)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
