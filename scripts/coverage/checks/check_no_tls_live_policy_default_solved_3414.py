#!/usr/bin/env python3
"""Issue #3414: no-TLS live_policy must not default SOLVED; depth==0 Quiet refuse.

Residual of #3379: face-only fill left solve_status=0 / linear_ok=true
after outermost TLS clear. Production IR/JIT at depth==0 returned true
on Quiet / unbound last-proof.

Contract:
  AC1 Production/Full + no live TC → not default SOLVED; deny via
      existing solve (TIMEOUT-class) unless Stamped + gen match + faces clear
  AC2 (superseded by #3439): depth computed first; depth==0 short-circuits
      unconditionally BEFORE any stale-atomic pre-checks (chaos test
      cross-contamination, warm line 401 regression); Reject + stamper==TLS
      + live-policy gate stay enforced at depth > 0
  AC3 Soft/Off early-return unchanged; no extra CS walk when quiet
  AC4 no new query key / reason code / g_3414_*; reuse
      g_linear_fast_path_elide_blocked_production_total
  AC5 extend test_typed_audit_commit_readiness_live_policy +
      test_ir_typed_entry_proof_authority + test_commit_readiness_score;
      linter after #3224; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    t3379 = _read("tests/compiler/test_typed_audit_commit_readiness_live_policy.cpp")
    t3305 = _read("tests/compiler/test_ir_typed_entry_proof_authority.cpp")
    t2553 = _read("tests/compiler/test_commit_readiness_score.cpp")
    build = _read("build.py")

    must("kNoTlsLivePolicyDefaultSolvedIssue = 3414", "AC1 stamp", tma)
    lp = tma.find("inline CommitReadinessInput commit_readiness_live_policy() noexcept {")
    lp_win = tma[lp : lp + 12000] if lp >= 0 else ""
    must("kTypeLinearProofOutcomeStamped", "AC1 Stamped gate", lp_win)
    must("in.solve_status = 2", "AC1 TIMEOUT-class deny", lp_win)
    must("else if (prod || full)", "AC1 Production/Full only", lp_win)
    must("g_tls_audit_commit_readiness_evaluator", "AC1 TLS", lp_win)

    # AC2 (post-#3439): depth computed first; depth==0 short-circuits
    # unconditionally BEFORE any stale-atomic pre-checks (chaos test
    # cross-contamination regression, warm line 401). Reject/invalidate/
    # stamper==TLS/live-policy stay enforced at depth > 0.
    pred = tma.find("ir_typed_entry_commit_readiness_ok() noexcept")
    pred_win = tma[pred : pred + 3800] if pred >= 0 else ""
    must("aura_evaluator_mutation_boundary_depth()", "AC2 depth ABI", pred_win)
    must("if (depth == 0)", "AC2 depth==0", pred_win)
    depth_pos = pred_win.find("if (depth == 0)")
    ret_pos = pred_win.find("return true", depth_pos if depth_pos >= 0 else 0)
    if depth_pos < 0 or ret_pos < 0:
        fails.append("AC2: depth==0 bypass missing")
    else:
        prefix = pred_win[:ret_pos]
        for bad in ("kTypeLinearProofOutcomeReject", "abort_or_mid_abort_blocks_elision()"):
            if bad in prefix:
                fails.append(f"AC2: stale-atomic pre-check before depth==0 bypass: {bad}")
    must("kTypeLinearProofOutcomeReject", "AC2 Reject kept (depth > 0)", pred_win)
    must("last_proof_bound_to_current_eval()", "AC2 stamper==TLS kept (depth > 0)", pred_win)
    must("commit_readiness(commit_readiness_live_policy())", "AC2 live-policy final gate", pred_win)

    must(
        "if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))",
        "AC3 Soft guard",
        pred_win,
    )
    arm = ""
    arm_pos = lp_win.find("else if (prod || full)")
    if arm_pos >= 0:
        arm = lp_win[arm_pos : arm_pos + 1600]
    if "cs.solve(" in arm or "constraint_system().solve" in arm:
        fails.append("AC3: no-TC arm must not walk ConstraintSystem")

    must("g_linear_fast_path_elide_blocked_production_total", "AC4 reuse", pred_win)
    if "g_3414_" in tma:
        fails.append("AC4: new g_3414_* counter")
    if "schema-3414" in tma:
        fails.append("AC4: new schema-3414")
    if "query:no-tls" in tma:
        fails.append("AC4: new query key")

    must("ac3414_no_tls_default_solved_refused", "AC5 score test", t2553)
    must("kNoTlsLivePolicyDefaultSolvedIssue = 3414", "AC5 3379 suite", t3379)
    must("kTypeLinearProofOutcomeStamped", "AC5 3305 suite", t3305)
    must("check_no_tls_live_policy_default_solved_3414", "AC5 build.py", build)
    prev = build.find("check_ir_typed_entry_commit_readiness_3224")
    ours = build.find("check_no_tls_live_policy_default_solved_3414")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3224")
    if (ROOT / "tests" / "compiler" / "test_issue_3414.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3414.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3414.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3414.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3414-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3414 no_tls_live_policy_default_solved:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3414 no_tls_live_policy_default_solved: Quiet no-TC denied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
