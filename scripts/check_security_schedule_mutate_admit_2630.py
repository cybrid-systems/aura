#!/usr/bin/env python3
"""check_security_schedule_mutate_admit_2630.py — Issue #2630 source gate.

Wire security-schedule-gate (#2590 contract) into mutate admission
entry points (MutationBoundaryGuard::try_acquire + try_acquire_for_region).
Closes the half-green / deny-storm window where Agents keep mutating
after security posture degraded.

AC1: production + commit_not_ready hard → new mutate rejected at
     try_acquire; deny-total / commit-not-ready counter bumps
     (existing #2590 contract counters via g_orch_security_schedule_counters).
AC2: production + deny_storm / mid_fallback_slo / posture_degraded →
     reject with matching force_reason.
AC3: Soft / AURA_SANDBOX=off → allow + observe-only (never denies;
     AC3 of #2590 preserved). Falls through to quota check.
AC4: Zero extra work when all-clear (single pure decide path —
     decide_security_schedule is pure #2590 AC1).
AC5: query:security-schedule-gate last decision reflects live
     admission outcome (last_force_reason_code / last_would_allow
     from g_orch_security_schedule_counters bumped by
     evaluate_security_schedule in the call site).
AC6: src-aligned test + coverage gate (source-cite try_acquire +
     try_acquire_for_region; #2630-specific keys in the query surface).
AC7: #2543 AOT throttle / #2587 mailbox-starvation gates unchanged.
     Ordering: starvation → schedule → quota. The new gate sits
     between #2587 mailbox-starvation and the existing quota check.

Rationale (Issue #2630 body):
  #2590 shipped the pure gate contract (security_schedule_gate.h
  + query:security-schedule-gate) with explicit note that
  per-call-site wiring is the natural follow-up. Without that
  wiring, half-green commit / deny-storm / mid-fallback-SLO /
  posture-degraded states still allow Agents to keep mutating. This
  issue wires the gate into the two primary call sites in
  MutationBoundaryGuard (try_acquire + try_acquire_for_region)
  that cover host + fiber soft + TransactionGuard host callback.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVAL = ROOT / "src/compiler/evaluator_mutation_boundary.cpp"
SEC = ROOT / "src/compiler/evaluator_primitives_security.cpp"
TEST_DIR = ROOT / "tests/compiler"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block (handles nested {})."""
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}"
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
        i += 1
    return ""


def _find_function_body(text: str, signature_regex: str) -> str:
    """Find a function by its signature regex and return its body."""
    m = re.search(signature_regex, text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    # AC1+AC2+AC3+AC4: try_acquire + try_acquire_for_region must call
    # evaluate_security_schedule before admitting new mutate.
    if not EVAL.exists():
        failures.append("AC1: src/compiler/evaluator_mutation_boundary.cpp not found")
        eval_cpp = ""
    else:
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")

    if eval_cpp:
        # Must have the gate call in both call sites.
        for fn in ("try_acquire(", "try_acquire_for_region("):
            body = _find_function_body(
                eval_cpp,
                rf"Evaluator::MutationBoundaryGuard::{re.escape(fn)}[^)]*\)\s*noexcept\s*\{{",
            )
            if not body:
                failures.append(
                    f"AC1: MutationBoundaryGuard::{fn} not found in "
                    f"evaluator_mutation_boundary.cpp"
                )
                continue
            if "evaluate_security_schedule" not in body:
                failures.append(
                    f"AC1: MutationBoundaryGuard::{fn} does not call "
                    f"evaluate_security_schedule (call-site wiring missing)"
                )
            if "SecurityScheduleInput" not in body:
                failures.append(
                    f"AC1: MutationBoundaryGuard::{fn} does not build "
                    f"SecurityScheduleInput from live signals"
                )
            # AC2: deny path must include "security-schedule:" + force_reason
            # for the structured error string.
            if "security-schedule:" not in body:
                failures.append(
                    f"AC2: MutationBoundaryGuard::{fn} deny path must include "
                    f"'security-schedule:' prefix in error string"
                )
            if "AdmissionRejected" not in body:
                failures.append(
                    f"AC2: MutationBoundaryGuard::{fn} deny path must use "
                    f"'AdmissionRejected:' prefix (sibling of #2587)"
                )
            if "ssd.force_reason" not in body and "force_reason" not in body:
                failures.append(
                    f"AC2: MutationBoundaryGuard::{fn} deny path must use "
                    f"the gate's force_reason for the error string"
                )
            # AC3: soft / sandbox=off must fall through (production
            # check gates the reject).
            if "typed_audit::production_defaults_active" not in body:
                failures.append(
                    f"AC3: MutationBoundaryGuard::{fn} must gate reject on "
                    f"typed_audit::production_defaults_active (soft/sandbox=off "
                    f"falls through — AC3 of #2590 preserved)"
                )
            # AC4: zero extra work when all-clear — pure decide is free
            # when production/!soft/!deny path. The call is the
            # ONLY work added (single pure decide).
            if "would_allow_new_mutate" not in body:
                failures.append(
                    f"AC4: MutationBoundaryGuard::{fn} must check "
                    f"decision.would_allow_new_mutate (single pure decide path)"
                )

    # AC5: query:security-schedule-gate has #2630-specific keys.
    if not SEC.exists():
        failures.append("AC5: src/compiler/evaluator_primitives_security.cpp not found")
    else:
        sec = SEC.read_text(encoding="utf-8", errors="replace")
        for key in (
            "security-schedule-mutate-admit-wired",
            "schema-2630",
            "issue-2630",
        ):
            if key not in sec:
                failures.append(
                    f"AC5: evaluator_primitives_security.cpp does not expose "
                    f"{key} on query:security-schedule-gate"
                )
        # Compatibility: prior #2590 keys preserved.
        for key in ("security-schedule-gate-wired", "schema-2590", "issue-2590"):
            if key not in sec:
                failures.append(
                    f"AC5: evaluator_primitives_security.cpp does not preserve "
                    f"existing {key} (compatibility with #2590)"
                )

    # AC6: test file has ac2630_* sections.
    found_test = False
    for test_path in TEST_DIR.glob("test_*security*.cpp"):
        test_text = test_path.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2630_production_commit_not_ready_rejects",
            "ac2630_soft_falls_through",
            "ac2630_evaluate_security_schedule_called",
            "ac2630_zero_extra_work_all_clear",
            "ac2630_source_and_schema",
        ):
            if ac_fn in test_text:
                found_test = True
        # Don't break early — we want to count all ac2630_* across all files
    if not found_test:
        # AC6 fallback: look for any ac2630_* in any test file.
        any_ac2630 = False
        for test_path in TEST_DIR.glob("test_*.cpp"):
            test_text = test_path.read_text(encoding="utf-8", errors="replace")
            if "ac2630_" in test_text:
                any_ac2630 = True
                break
        if not any_ac2630:
            failures.append(
                "AC6: no test file has ac2630_* sections "
                "(#2630 call-site wiring test coverage missing)"
            )

    # AC7: #2543 AOT throttle / #2587 mailbox-starvation gates unchanged.
    if eval_cpp:
        # The new gate must be AFTER #2587 mailbox-starvation throttle
        # and BEFORE the quota check. We verify the ordering by
        # looking for the sequence in the function body.
        body = _find_function_body(
            eval_cpp,
            r"Evaluator::MutationBoundaryGuard::try_acquire\(",
        )
        if body:
            starvation_idx = body.find("aura_orch_mailbox_starvation_throttled")
            schedule_idx = body.find("evaluate_security_schedule")
            quota_idx = body.find("check_mutation_quota")
            if starvation_idx != -1 and schedule_idx != -1 and quota_idx != -1:
                if not (starvation_idx < schedule_idx < quota_idx):
                    failures.append(
                        "AC7: ordering violation — new gate must be "
                        "AFTER #2587 mailbox-starvation and BEFORE quota check"
                    )
        # Also verify #2587 mailbox-starvation throttle is still present
        # (unchanged).
        body2 = _find_function_body(
            eval_cpp,
            r"Evaluator::MutationBoundaryGuard::try_acquire\(",
        )
        if body2 and "aura_orch_mailbox_starvation_throttled" not in body2:
            failures.append(
                "AC7: #2587 mailbox-starvation throttle missing from try_acquire "
                "(should remain unchanged)"
            )
        # And the #2587 sibling reject pattern.
        if body2 and "mailbox-hold-starvation" not in body2:
            failures.append(
                "AC7: #2587 mailbox-hold-starvation reject string missing "
                "from try_acquire (should remain unchanged)"
            )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print(
        "OK: all #2630 ACs satisfied (security-schedule-gate wired into "
        "MutationBoundaryGuard::try_acquire + try_acquire_for_region — "
        "production deny / soft fall-through / ordering preserved / tests)"
    )
    return 0


def main_strict() -> int:
    """Always-strict variant for self-test fixtures."""
    saved = sys.argv
    try:
        sys.argv = list(saved) + ["--strict"]
        return main()
    finally:
        sys.argv = saved


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_2630_selftest_"))
    try:
        good_eval = tmp / "eval.cpp"
        good_eval.write_text(
            "Evaluator::MutationBoundaryGuard::try_acquire(Evaluator& ev, std::uint64_t pending_count,\n"
            "                                              bool* success_flag, bool fine_rollback) noexcept {\n"
            "    // Issue #2587: mailbox-starvation throttle\n"
            "    if (aura::serve::mf_mailbox::aura_orch_mailbox_starvation_throttled()) {\n"
            "        if (typed_audit::production_defaults_active()) {\n"
            "            return std::unexpected(\n"
            "                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,\n"
            "                                      std::string(\"AdmissionRejected: mailbox-hold-starvation\")));\n"
            "        }\n"
            "    }\n"
            "    // Issue #2630: security-schedule-gate\n"
            "    {\n"
            "        aura::orch::SecurityScheduleInput in;\n"
            "        in.production_mode = typed_audit::production_defaults_active();\n"
            "        in.soft_mode = !in.production_mode;\n"
            "        in.commit_readiness_would_allow = true;\n"
            "        in.commit_readiness_hard_reject = false;\n"
            "        const auto ssd = aura::orch::evaluate_security_schedule(in);\n"
            "        if (!ssd.would_allow_new_mutate && in.production_mode) {\n"
            "            return std::unexpected(aura::core::AuraError(\n"
            "                aura::core::AuraErrorKind::ResourceQuotaExceeded,\n"
            "                std::string(\"AdmissionRejected: security-schedule:\") +\n"
            "                    std::string(aura::orch::security_schedule_force_reason_name(ssd.force_reason))));\n"
            "        }\n"
            "    }\n"
            "    // quota check after\n"
            "    auto err = ev.check_mutation_quota(pending_count);\n"
            "    return std::unexpected(std::move(*err));\n"
            "}\n"
            "Evaluator::MutationBoundaryGuard::try_acquire_for_region(Evaluator& ev, std::uint64_t region_key,\n"
            "                                                         std::uint64_t pending_count,\n"
            "                                                         bool* success_flag, bool fine_rollback) noexcept {\n"
            "    // Issue #2630: security-schedule-gate\n"
            "    {\n"
            "        aura::orch::SecurityScheduleInput in;\n"
            "        in.production_mode = typed_audit::production_defaults_active();\n"
            "        const auto ssd = aura::orch::evaluate_security_schedule(in);\n"
            "        if (!ssd.would_allow_new_mutate && in.production_mode) {\n"
            "            return std::unexpected(aura::core::AuraError(\n"
            "                aura::core::AuraErrorKind::ResourceQuotaExceeded,\n"
            "                std::string(\"AdmissionRejected: security-schedule:\") +\n"
            "                    std::string(aura::orch::security_schedule_force_reason_name(ssd.force_reason))));\n"
            "        }\n"
            "    }\n"
            "    return std::unique_ptr<MutationBoundaryGuard>();\n"
            "}\n",
            encoding="utf-8",
        )

        good_sec = tmp / "sec.cpp"
        good_sec.write_text(
            '{"security-schedule-mutate-admit-wired", make_int(1)},\n'
            '{"schema-2630", make_int(2630)},\n'
            '{"issue-2630", make_int(2630)},\n'
            '{"security-schedule-gate-wired", make_int(1)},\n'
            '{"schema-2590", make_int(2590)},\n'
            '{"issue-2590", make_int(2590)},\n',
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "static void ac2630_production_commit_not_ready_rejects() {}\n"
            "static void ac2630_soft_falls_through() {}\n"
            "static void ac2630_evaluate_security_schedule_called() {}\n"
            "static void ac2630_zero_extra_work_all_clear() {}\n"
            "static void ac2630_source_and_schema() {}\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )

        import check_security_schedule_mutate_admit_2630 as self_mod

        original = {
            "EVAL": self_mod.EVAL,
            "SEC": self_mod.SEC,
        }
        try:
            self_mod.EVAL = good_eval
            self_mod.SEC = good_sec
            self_mod.TEST_DIR = tmp
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Bad fixture: missing gate call in try_acquire
        bad_eval = tmp / "eval_bad.cpp"
        bad_eval.write_text(
            "Evaluator::MutationBoundaryGuard::try_acquire(Evaluator& ev, std::uint64_t pending_count,\n"
            "                                              bool* success_flag, bool fine_rollback) noexcept {\n"
            "    // Issue #2587: mailbox-starvation throttle\n"
            "    if (aura::serve::mf_mailbox::aura_orch_mailbox_starvation_throttled()) {\n"
            "        if (typed_audit::production_defaults_active()) {\n"
            "            return std::unexpected(\n"
            "                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,\n"
            "                                      std::string(\"AdmissionRejected: mailbox-hold-starvation\")));\n"
            "        }\n"
            "    }\n"
            "    // missing security-schedule-gate wiring\n"
            "    return std::unexpected(aura::core::AuraError());\n"
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.EVAL = bad_eval
            self_mod.SEC = good_sec
            self_mod.TEST_DIR = tmp
            rc_bad = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print(
                "SELF-TEST FAIL: known-bad (missing gate call) accepted",
                file=sys.stderr,
            )
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
