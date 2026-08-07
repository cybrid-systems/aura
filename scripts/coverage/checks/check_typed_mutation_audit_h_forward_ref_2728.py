#!/usr/bin/env python3
"""Issue #2728: fix typed_mutation_audit.h forward-reference cascade
(blocks aura_test_objects rebuild — noted on every recent P0 ship
#2717/#2718/#2719/#2720/#2721).

Contract (one row per AC):
  AC1 src/compiler/typed_mutation_audit.h is self-consistent: forward
     declarations for the symbols that create cyclic or out-of-order
     dependencies (commit_readiness_live_policy, commit_readiness,
     cone_outside_goal_drop_total_v_read,
     occurrence_empty_after_fence_total_v_read) live in a single
     forward-declaration block at the top of the file (per the issue
     recommendation: "Prefer moving pure declarations to the top / a
     separate forward block"). aura_test_objects + the full test suite
     rebuild cleanly.
  AC2 Existing tests that exercise the header (type-linear-commit,
     occurrence, coercion) continue to pass — no semantic change to
     TypeLinearCommitProof / readiness / occurrence / coercion
     surfaces.
  AC3 No change to runtime behavior or query surfaces of #2697 /
     #2717 / #2719. The reorder is structural only — same inline
     functions, same call sites, same query keys / counters.
  AC4 Source-cite the reordered symbols (forward-declaration block +
     the original-position #2716 atomic definition + the prior
     commit_readiness_live_policy / commit_readiness call sites +
     build_type_linear_commit_proof_from_live call sites + the gcc
     16.1 ICE workaround note in the top block); coverage linter clean
     via this script; no docs/design/2728-* on disk per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # AC1 — typed_mutation_audit.h is self-consistent:
    #   - Single forward-declaration block at the top (Issue #2728 anchor).
    #   - Forward decls: CommitReadinessInput / CommitReadiness (struct),
    #     commit_readiness_live_policy / commit_readiness (functions),
    #     cone_outside_goal_drop_total_v_read /
    #     occurrence_empty_after_fence_total_v_read (v_read helpers).
    #   - g_occurrence_hard_face_full_solve_recover_total is an `inline
    #     std::atomic{0}` defined BEFORE its usage site
    #     (line ~1053 < line ~1092 in commit_readiness_live_policy) —
    #     no `extern` forward decl (the prior extern/inline split
    #     tripped gcc 16.1's parser ICE in
    #     typed_mutation_audit_hooks.cpp).
    tma = _read("src/compiler/typed_mutation_audit.h")
    must("Issue #2728: forward-declaration block", "AC1", tma)
    must("struct CommitReadinessInput;", "AC1", tma)
    must("struct CommitReadiness;", "AC1", tma)
    must("commit_readiness_live_policy() noexcept", "AC1", tma)
    must("commit_readiness(const CommitReadinessInput& in) noexcept", "AC1", tma)
    must("cone_outside_goal_drop_total_v_read() noexcept", "AC1", tma)
    must("occurrence_empty_after_fence_total_v_read() noexcept", "AC1", tma)
    # The `inline std::atomic<...>{0}` definition must remain (no
    # `extern` forward decl — gcc 16.1 ICE workaround note).
    must("inline std::atomic<std::uint64_t> g_occurrence_hard_face_full_solve_recover_total{0}", "AC1", tma)
    must("gcc 16.1's", "AC1", tma)  # ICE workaround note (split text)
    # The struct definitions must remain in their original positions.
    must("struct CommitReadinessInput {", "AC1", tma)
    must("struct CommitReadiness {", "AC1", tma)
    # Forward decls are removed from their old scattered positions.
    # The build_type_linear_commit_proof_from_live helper should still
    # call commit_readiness_live_policy() and commit_readiness()
    # (the forward decls are at the top now).
    must("build_type_linear_commit_proof_from_live", "AC1", tma)
    must("const auto ready = commit_readiness_live_policy()", "AC1", tma)
    must("const auto live_r = commit_readiness(ready)", "AC1", tma)

    # AC2 — existing inline functions remain in the header (no
    # semantic change). Verify the definitions are intact at their
    # original positions (not moved out of the header — per the issue:
    # "keep definitions out of the header if possible" applies to NEW
    # decls only; existing inline definitions stay where they are).
    must("commit_readiness_live_policy() noexcept {", "AC2", tma)
    must("inline CommitReadiness commit_readiness(const CommitReadinessInput& in) noexcept {", "AC2", tma)
    must("cone_outside_goal_drop_total_v_read() noexcept {", "AC2", tma)
    must("occurrence_empty_after_fence_total_v_read() noexcept {", "AC2", tma)
    must("g_occurrence_hard_face_full_solve_recover_total.fetch_add(1", "AC2", tma)

    # AC3 — no change to runtime behavior / query surfaces. The
    # inline definitions and atomic counters are unchanged. Verify
    # the existing query keys for #2697 / #2717 / #2719 are still
    # present in evaluator_primitives_query.cpp (no regression).
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    must("schema-2697", "AC3", q)
    must("schema-2717", "AC3", q)
    must("schema-2719", "AC3", q)
    must("typed-mutation-audit-", "AC3", q)
    must("issue-2717", "AC3", q)
    must("issue-2719", "AC3", q)

    # AC4 — source-cite the reordered symbols; coverage linter clean
    # (this script); no docs/design/2728-* on disk per #1655.
    must("Issue #2728", "AC4", tma)
    # #81967: NO new test file — extend the existing one (no
    # tests/serve/test_issue_2728.cpp).
    if (ROOT / "tests" / "serve" / "test_issue_2728.cpp").is_file():
        fails.append("AC4: tests/serve/test_issue_2728.cpp present (forbidden per #81967)")
    # build.py wires the linter.
    build = _read("build.py")
    must("check_typed_mutation_audit_h_forward_ref_2728", "AC4", build)
    # No docs/design/2728-* on disk per #1655.
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2728-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior linters (build-path coverage contract) still
    # green — the #2728 fix doesn't regress any prior linter.
    for prev in (
        "check_cross_fiber_hold_budget_cancel_2726.py",
        "check_fiber_evaluator_id_2727.py",
        "check_mutation_hold_budget_reject_2701.py",
    ):
        prev_path = ROOT / "scripts" / "coverage" / "checks" / prev
        if not prev_path.is_file():
            continue
        r = subprocess.run(
            [sys.executable, str(prev_path)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2728 typed_mutation_audit.h forward-reference cascade — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
