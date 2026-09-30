#!/usr/bin/env python3
"""Issue #4239 source-cite gate: require_effect hard face refuses on
join(0)==0 — the TypeLinear proof stamp must NOT resurrect the mid.

Hole: Evaluator::require_effect's hard face joins via the #3966 SSOT
(join_audit_and_se_mid(0)). When that returns 0, resolve_audit_mutation_id
has already emitted the canonical mid=0 "mid-fallback-refused" SE
(#2836/#3054). The function then still fell through to
last_type_linear_commit_proof_stamp_v_read() and current_mutation_epoch()
before fail-closed — so a leftover proof stamp P from a prior outermost
success resurrected the join key and EffectAllow / grant-bind ran on P
while the refuse SE landed on mid=0. One action, two join keys: Agent
query:security-audit by mid=0 sees the refuse, by mid=P sees the allow
(#4098: the TypeLinear proof stamp is not the session / TypedMid join
key). production_deny_se_mid (IsolationDeny face, same TU) already
returns the join only — no resurrection.

Fix shape (no second mid resolver, no new metrics bus):
  - hard face: mid = join_audit_and_se_mid(0); mid==0 → absolute refuse
    (return false, zero side effect). The refuse SE is resolve's job —
    exactly one, unchanged.
  - epoch!=0 still allows: join already returns the WorkspaceEpoch
    mutation via resolve (#3296), so the refuse is not over-broad.
  - Soft / Off arm unchanged: proof → epoch → mid=1 observe stamp
    (#2493 AC4, #3594 AC3 contract).
  - Sampled-leftover behavior untouched: should_audit stays forced under
    production (#4173) — this issue is join resurrection only.

Contract (one row per AC):
  AC1  evaluator_security.cpp require_effect hard face: cites #4239,
       join_audit_and_se_mid(0) then mid==0 absolute refuse; the
       proof-stamp resurrection chain is gone (exactly one
       last_type_linear_commit_proof_stamp_v_read read remains in the
       function — the Soft SSOT pre-compute)
  AC2  alignment: production_deny_se_mid keeps the join-only production
       face; Soft arm (pre-compute + epoch + else-if mid==0 → mid=1)
       intact; no second resolver / no new refuse emitter in the TU
       (no emit_security_event_durable inside require_effect)
  AC3  runtime ACs dispatched in tests/compiler/test_require_effect_live_mid.cpp
       (ac4239_1..ac4239_5 defined and called from
       run_test_std_ffi_per_call_3725 — the dispatched runner); no
       tests/**/test_issue_4239.cpp; no docs/design/4239-*
  AC4  build.py wiring + scripts/coverage/root_check_allowlist.txt entry

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def count(hay: str, needle: str) -> int:
        n = 0
        p = hay.find(needle)
        while p != -1:
            n += 1
            p = hay.find(needle, p + 1)
        return n

    def window(src: str, begin: str, end: str, label: str) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + 6000)]

    sec_src = _read("src/compiler/evaluator_security.cpp")
    test_src = _read("tests/compiler/test_require_effect_live_mid.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: hard face absolute refuse, no resurrection ──
    re_fn = window(
        sec_src,
        "bool Evaluator::require_effect(",
        "Issue #2658: thin helper",
        "AC1",
    )
    must("#4239", "AC1 cite", re_fn)
    # Issue #4241 compose: the probe face resolves via peek_audit_mutation_id
    # (pure join read — same cascade, silent); the eager join→resolve emit on
    # this read-style face is superseded. Commit/deny faces keep join/resolve.
    must("typed_audit::peek_audit_mutation_id(0)", "AC1 read-style peek resolver (#4241)", re_fn)
    absent("join_audit_and_se_mid(0)", "AC1 no eager join emit on the probe face (#4241)", re_fn)
    must("return false; // fail-closed, zero side effect (no refuse SE, #4241)", "AC1 refuse", re_fn)
    # The proof stamp is read exactly once in the function — the Soft SSOT
    # pre-compute. The hard-face resurrection chain is gone.
    proof_reads = count(re_fn, "last_type_linear_commit_proof_stamp_v_read")
    if proof_reads != 1:
        fails.append(f"AC1: proof-stamp reads in require_effect = {proof_reads}, want 1")
    # Epoch resurrection on the hard face is gone too: the only
    # current_mutation_epoch() read left is the Soft pre-compute.
    epoch_reads = count(re_fn, "::aura::core::current_mutation_epoch()")
    if epoch_reads != 1:
        fails.append(f"AC1: epoch reads in require_effect = {epoch_reads}, want 1")
    # Ordering: the refuse fires inside the hard branch, before the Soft
    # else-if mid=1 arm.
    hard_at = re_fn.find("if (hard) {")
    refuse_at = re_fn.find("return false; // fail-closed, zero side effect (no refuse SE, #4241)")
    soft_at = re_fn.find("mid = 1; // Soft only")
    if hard_at == -1 or refuse_at == -1 or soft_at == -1 or not (hard_at < refuse_at < soft_at):
        fails.append("AC1: refuse not ordered inside the hard branch (hard < refuse < soft)")

    # ── AC2: deny-face alignment + Soft arm intact + no second resolver ──
    deny_fn = window(
        sec_src,
        "static std::uint64_t production_deny_se_mid(",
        "aura_isolation_deny_se_mid",
        "AC2",
    )
    must("return join_audit_and_se_mid(caller_mid);", "AC2 deny join-only", deny_fn)
    must(
        "std::uint64_t mid = typed_audit::last_type_linear_commit_proof_stamp_v_read();", "AC2 soft pre-compute", re_fn
    )
    must("if (mid == 0)\n        mid = ::aura::core::current_mutation_epoch();", "AC2 soft epoch arm", re_fn)
    must("else if (mid == 0) {", "AC2 soft else-if", re_fn)
    must("mid = 1; // Soft only: non-zero join stamp (process origin)", "AC2 soft mid=1", re_fn)
    # The refuse SE stays resolve's job (single emitter in typed_mutation_audit.h):
    # require_effect itself emits nothing.
    absent("emit_security_event_durable", "AC2", re_fn)
    absent("schema-4239", "AC2", sec_src)
    tma_src = _read("src/compiler/typed_mutation_audit.h")
    must("g_tls_mid_fallback_refuse_se_emitted", "AC2 single-refuse TLS", tma_src)

    # ── AC3: runtime ACs dispatched in the batch runner ──
    for fn in (
        "ac4239_1_join_zero_refuses_no_proof_grant_allow",
        "ac4239_2_refuse_se_joins_mid_zero_only",
        "ac4239_3_epoch_join_still_allows",
        "ac4239_4_live_guard_after_refuse_shares_session",
        "ac4239_5_soft_observe_and_source",
    ):
        must(f"static void {fn}()", "AC3 def", test_src)
        must(f"{fn}();", "AC3 call", test_src)
    must("Issue #4239: join==0 refuse", "AC3 runner header", test_src)
    # Calls live in the DISPATCHED runner (batch TU dispatches
    # run_test_std_ffi_per_call_3725, #3725/#3966 precedent).
    runner = window(
        test_src,
        "int run_test_std_ffi_per_call_3725() {",
        "return aura::test::g_failed ? 1 : 0;",
        "AC3-runner",
    )
    must("ac4239_1_join_zero_refuses_no_proof_grant_allow();", "AC3 runner", runner)
    must("ac4239_5_soft_observe_and_source();", "AC3 runner", runner)
    # No invent: no new issue test file, no docs/design markdown.
    invent = list(ROOT.glob("tests/**/test_issue_4239.cpp"))
    if invent:
        fails.append(f"AC3: forbidden new test file {invent[0]}")
    design = list(ROOT.glob("docs/design/4239-*"))
    if design:
        fails.append(f"AC3: forbidden docs/design file {design[0]}")

    # ── AC4: wiring ──
    must("check_require_effect_join_refuse_4239", "AC4", build_src)
    must("check_require_effect_join_refuse_4239.py", "AC4", allow_src)

    if fails:
        print("check_require_effect_join_refuse_4239: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_require_effect_join_refuse_4239: ok (AC1–AC4)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
