#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4037: mutate:set-agent-fingerprint forged MutationRecord.author_fingerprint
# with no capability — the prim was SECURITY_EXEMPT and never called
# require_effect, so any agent could label the next typed atomic batch with any
# fingerprint (TypedTransactionGuard copies current_agent_fingerprint onto every
# sub-mutation). The fix makes the NON-ZERO store a TenantAdmin identity write,
# enforced IN BODY and conditionally: a dispatch-level require cannot see the
# arg and would gate the set-to-0 clear, so the conditional in-body require is
# the operative spec (single choke; dispatch skips via effect_enforced_in_body —
# no double consumption of single-use grants, same note as c-*).
#
# AC1 — meta contract: the PrimMeta block for the prim declares
#       required_effects = kEffectTenantAdmin and effect_enforced_in_body = true
#       and no longer assigns security_exempt (guard_exempt stays true —
#       metadata-only, no AST write).
# AC2 — gated direction: the body requires kEffectTenantAdmin for the NON-ZERO
#       store; the deny returns the capability error BEFORE the store call
#       (require_effect precedes set_current_agent_fingerprint, store unchanged
#       on deny).
# AC3 — ungated clear: the require sits inside the `fp != 0` conditional —
#       set-to-0 never reaches require_effect (#4037 Verify bullet 4: a denied
#       process must not pin a forged label forever; same shape as
#       hygiene:set-allow-macro-mutate! #f ungated).
# AC4 — Soft/Off ungated: the condition includes the effect_sandbox_mode() != 0
#       guard so the Off face keeps the zero-cost ungated store (production
#       face = Restricted/Strict is the enforced direction).
# AC5 — deny surface: the deny path returns a "capability-effect-deny" error
#       (the same string require_effect emits as the SE reason for a
#       TenantAdmin-only requirement).
# AC6 — allowlist row removed: tests/side-effect-security-allowlist.txt no
#       longer lists the prim (it is no longer SECURITY_EXEMPT).
# AC7 — dispatch stays single-choke: the meta window keeps effect_enforced_in_body
#       (dispatch skips require_effect — exactly one consumption per call).
# AC8 — ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no test_issue_4037.cpp; no docs/design/4037-* (per #1655).
#
# Self-test:
#   python3 scripts/check_agent_fingerprint_gate_4037.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MUTATE = "src/compiler/evaluator_primitives_mutate.cpp"
ALLOWLIST = "tests/side-effect-security-allowlist.txt"
HOST_TEST = "tests/compiler/test_dispatch_required_effects.cpp"
PRIM = "mutate:set-agent-fingerprint"


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring search
    does not false-positive on prose. Cheap state machine (same shape as the
    #4036 linter)."""
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


def _normalize(src: str) -> str:
    return re.sub(r"\s+", " ", src)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []
    mut_raw = _read(MUTATE)
    if not mut_raw:
        print(f"FAIL: {MUTATE} missing")
        return 1
    mut = _strip_cpp_comments(mut_raw)

    # The full registration region: from the raw add() to the meta stamp.
    add_at = mut.find(f'add("{PRIM}"')
    meta_at = mut.find("PrimMeta ex{};", add_at if add_at >= 0 else 0)
    meta_end = mut.find("set_meta_for_name", meta_at if meta_at >= 0 else 0)
    if add_at < 0 or meta_at < 0 or meta_end < 0:
        fails.append("AC1: registration region (raw add + PrimMeta ex block) not located")
    else:
        body = _normalize(mut[add_at:meta_at])
        meta = _normalize(mut[meta_at:meta_end])

        # AC1 — meta contract.
        if "ex.required_effects = ::aura::compiler::security::kEffectTenantAdmin;" not in meta:
            fails.append("AC1: meta must declare required_effects = kEffectTenantAdmin")
        if "ex.effect_enforced_in_body = true;" not in meta:
            fails.append("AC1: meta must declare effect_enforced_in_body = true")
        if "ex.security_exempt" in meta:
            fails.append("AC1: meta must NOT assign security_exempt anymore (#4037)")
        if "ex.guard_exempt = true;" not in meta:
            fails.append("AC1: meta must keep guard_exempt = true (metadata-only)")

        # AC2 — gated direction: in-body require precedes the store.
        req_at = body.find("require_effect(kEffectTenantAdmin")
        store_at = body.find("set_current_agent_fingerprint")
        if req_at < 0:
            fails.append("AC2: body must require kEffectTenantAdmin in-body")
        if store_at < 0:
            fails.append("AC2: store call (set_current_agent_fingerprint) missing")
        if req_at >= 0 and store_at >= 0 and req_at > store_at:
            fails.append("AC2: require_effect must precede the store (deny leaves it unchanged)")

        # AC3 — ungated clear: the require is conditional on non-zero fp.
        if "if (fp != 0 &&" not in body:
            fails.append("AC3: require must sit inside the fp != 0 conditional (clear to 0 ungated)")

        # AC4 — Soft/Off ungated: sandbox-mode guard in the same condition.
        if "ev.effect_sandbox_mode() != 0" not in body:
            fails.append("AC4: condition must gate on effect_sandbox_mode() != 0 (Off stays ungated)")

        # AC5 — deny surface string.
        if '"capability-effect-deny"' not in body:
            fails.append('AC5: deny path must return a "capability-effect-deny" error')

        # AC7 — dispatch single choke (meta flag present is AC1; here pin the
        # body does NOT call the dispatch entry itself).
        if "invoke_prim_with_telemetry" in body:
            fails.append("AC7: body must not re-enter dispatch")

    # AC6 — allowlist row removed (entry lines only; prose comments that
    # mention the name document the removal, they are not allowlist rows).
    allow = _read(ALLOWLIST)
    if not allow:
        fails.append(f"AC6: {ALLOWLIST} missing")
    else:
        entries = [ln.strip() for ln in allow.splitlines() if ln.strip() and not ln.strip().startswith("#")]
        if any(ln == PRIM or ln.startswith(PRIM + " ") or ln.startswith(PRIM + "\t") for ln in entries):
            fails.append("AC6: allowlist still lists the prim — remove the SECURITY_EXEMPT row (#4037)")

    # AC8 — ACs live in the existing batch member; no new test file / docs.
    host = _read(HOST_TEST)
    if "#4037" not in host:
        fails.append(f"AC8: {HOST_TEST} must host the #4037 ACs")
    for pat in ("tests/**/test_issue_4037.cpp",):
        if list(ROOT.glob(pat)):
            fails.append("AC8: test_issue_4037.cpp created (per #81934 extend, don't create)")
    if list(ROOT.glob("docs/design/4037-*")):
        fails.append("AC8: docs/design/4037-* created (per #1655 no design docs)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("ok: agent-fingerprint gate (#4037) — 8/8 ACs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
