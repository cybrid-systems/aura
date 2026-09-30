#!/usr/bin/env python3
"""Issue #4241 source-cite gate: mid-fallback-refused emit ordering (peek vs resolve).

At tip, require_effect (read-style admission probe) resolved its join mid
through join_audit_and_se_mid → resolve_audit_mutation_id, which eagerly
emits the durable mid-fallback-refused SE (ring + WAL) on the production/Full
refuse face BEFORE the caller's zero-side-effect decide (return false on
join==0). Every pre-Guard / session-less probe (#3966 wrapper gate runs
before Guard enter) therefore wrote a durable refuse row as a side effect of
mid resolution — query:security-audit mid=0 fills with probe noise and real
refuse events get harder to triage.

Fix shape (no second model — the same resolve cascade, split):
  peek_audit_mutation_id()  — pure join read (composite pin → caller →
                              boundary → epoch); hard refuse face returns 0
                              SILENTLY, no SE, no refuse counters.
  resolve_audit_mutation_id() — delegates the cascade to peek and keeps the
                              TLS-guarded commit/deny-face emit (#2836/#3054).
  require_effect hard branch — peeks; on 0 fails closed without emitting.
  emit_invariant_deny_se    — deny face guarantees the refuse SE on mid=0
                              (hard-face resolve inside the mid==0 arm).

ACs:
  AC1  peek_audit_mutation_id exists ABOVE resolve_audit_mutation_id,
       mirrors the join cascade (composite pin first, then caller, boundary,
       epoch), contains no emit_security_event_durable / no
       "mid-fallback-refused" / no refuse counters.
  AC2  resolve_audit_mutation_id keeps the TLS-guarded refuse emit
       (g_tls_mid_fallback_refuse_se_emitted + "mid-fallback-refused" +
       audit_mid_fallback_refused_total) and delegates its cascade to peek.
  AC3  require_effect resolves via peek_audit_mutation_id(0), does NOT call
       join_audit_and_se_mid(0), and the fail-closed `return false` sits
       BEFORE check_and_record_effect; cites #4241.
  AC4  emit_invariant_deny_se's mid==0 arm calls resolve_audit_mutation_id
       under the hard face (deny faces emit; Soft stays silent), cites #4241.
  AC5  tests cite #4241 ACs; no tests/core/test_issue_4241.cpp; no
       docs/design/4241-* markdown; build.py + root_check_allowlist wired.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMA = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST = ROOT / "tests" / "compiler" / "test_require_effect_live_mid.cpp"


def _function_body(text: str, signature: str, end_marker: str) -> str:
    idx = text.find(signature)
    if idx < 0:
        return ""
    end = text.find(end_marker, idx + len(signature))
    return text[idx:end] if end > 0 else text[idx : idx + 3000]


def main() -> int:
    tma = TMA.read_text() if TMA.exists() else ""
    sec = SEC.read_text() if SEC.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: peek exists, is a pure read, and precedes resolve.
    peek_idx = tma.find("peek_audit_mutation_id(std::uint64_t caller_mid = 0) noexcept")
    resolve_idx = tma.find("resolve_audit_mutation_id(std::uint64_t caller_mid = 0) noexcept")
    peek_body = _function_body(
        tma, "peek_audit_mutation_id(std::uint64_t caller_mid = 0) noexcept", "// Issue #2493: canonical mid resolution"
    )
    good = (
        peek_idx >= 0
        and resolve_idx > peek_idx
        and "#4241" in peek_body
        and "g_tls_composite_batch_join_mid" in peek_body
        and "g_tls_boundary_audit_noted && g_tls_boundary_audit_mid != 0" in peek_body
        and "current_mutation_epoch" in peek_body
        and "emit_security_event_durable" not in peek_body
        and "mid-fallback-refused" not in peek_body
        and "audit_mid_fallback_refused_total" not in peek_body
        and "audit_mid_fallback_refuse_se_total" not in peek_body
    )
    report(
        "AC1",
        good,
        "peek_audit_mutation_id is a silent pure join read above resolve (composite → caller → boundary → epoch)",
    )

    # AC2: resolve keeps the TLS-guarded commit/deny-face emit, delegates to peek.
    resolve_body = _function_body(
        tma,
        "resolve_audit_mutation_id(std::uint64_t caller_mid = 0) noexcept",
        "// Issue #2814 M7: TLS link between trail Success and invariant enforcement.",
    )
    good = (
        resolve_idx >= 0
        and "peek_audit_mutation_id(caller_mid)" in resolve_body
        and "g_tls_mid_fallback_refuse_se_emitted" in resolve_body
        and '"mid-fallback-refused"' in resolve_body
        and "audit_mid_fallback_refused_total" in resolve_body
        and "audit_mid_fallback_refuse_se_total" in resolve_body
        and "emit_security_event_durable" in resolve_body
    )
    report(
        "AC2",
        good,
        "resolve delegates the cascade to peek and keeps the TLS-guarded mid-fallback-refused emit",
    )

    # AC3: require_effect peeks and fails closed before any effect record.
    req_body = _function_body(sec, "bool Evaluator::require_effect(", "bool Evaluator::require_effect_on_ref(")
    fail_idx = req_body.find("return false; // fail-closed, zero side effect (no refuse SE, #4241)")
    check_idx = req_body.find("check_and_record_effect(")
    good = (
        "#4241" in req_body
        and "peek_audit_mutation_id(0)" in req_body
        and "join_audit_and_se_mid(0)" not in req_body
        and fail_idx >= 0
        and check_idx > fail_idx >= 0
    )
    report(
        "AC3",
        good,
        "require_effect resolves via peek(0) and fails closed before check_and_record_effect",
    )

    # AC4: deny faces guarantee the refuse SE on mid=0 (hard face only).
    deny_body = _function_body(tma, "inline void emit_invariant_deny_se(", "// Issue #3066: pin one join mid")
    mid0_idx = deny_body.find("if (mid == 0) {")
    good = (
        "#4241" in deny_body
        and mid0_idx >= 0
        and "resolve_audit_mutation_id(0)" in deny_body
        and "production_defaults_active()" in deny_body
    )
    report(
        "AC4",
        good,
        "emit_invariant_deny_se mid==0 arm resolves (emit) under the hard face; Soft stays silent",
    )

    # AC5: tests cite the ACs; no issue-file/doc; build.py + allowlist wired.
    no_doc = not any((ROOT / "docs" / "design").glob("4241-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        all(f"#4241 AC{i}" in tst for i in range(1, 6))
        and "ac4241_1_probe_zero_side_effect" in tst
        and "ac4241_5_commit_face_wal_dual_write" in tst
        and not (ROOT / "tests" / "core" / "test_issue_4241.cpp").exists()
        and no_doc
        and "check_mid_refuse_ordering_4241.py" in build
        and "check_mid_refuse_ordering_4241.py" in allow
    )
    report("AC5", good, "tests cite #4241 ACs; no issue-file/doc; build.py + allowlist wired")

    print(f"check_mid_refuse_ordering_4241: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
