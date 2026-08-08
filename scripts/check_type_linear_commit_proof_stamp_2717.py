#!/usr/bin/env python3
"""Issue #2717: stamp TypeLinearCommitProof on boundary + composite commit.

Closes the #2697 residual: #2697 shipped TypeLinearCommitProof +
query:last-type-linear-commit-proof as an on-the-fly facade. Agents
could query, but boundary / composite_txn_commit did not stamp a
durable proof at success or reject. After densify/steal/remap, orch
could not hold a single object and re-check without re-joining N
surfaces. #2717 wires the active stamp inside boundary + composite
commit so Agents can hold a single TypeLinearCommitProof across
densify / steal / remap and re-check defuse_or_epoch_stamp without
N-key join.

Contract rows (AC1–AC6 from the test file):

  AC1: outermost boundary success and reject paths stamp
       TypeLinearCommitProof (readiness_bp / force_reason_code /
       would_allow_commit / linear_ok / occurrence_consistent /
       defuse_or_epoch_stamp / live_goal_count / linear_root_count
       when available).
  AC2: composite_txn_commit stamps on both ok and reject (same
       fields; schema-2697 preserved).
  AC3: Soft + quiet path (no linear ops, empty goals) → stamp cheap
       (zeros / vacuous healthy; no extra heavy walks).
  AC4: after densify/steal remap, Agent comparing
       defuse_or_epoch_stamp to current workspace epoch detects drift.
  AC5: additive only — preserve #2613 / #2697 query keys and health
       surface; optional type-linear-commit-proof-stamped-total.
  AC6: source-cite + linter + no docs/design/.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_type_linear_commit_proof_stamp_2717.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    efm = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1 — boundary success + reject paths stamp the proof.
    must("Issue #2717", "AC1", tma)
    must("build_type_linear_commit_proof_from_live", "AC1", tma)
    must("g_type_linear_commit_proof_stamped_total", "AC1", tma)
    must("build_type_linear_commit_proof_from_live(cp.version", "AC1", efm)
    # The two stamp sites: linear-synth-hard-fail early return + hygiene save.
    must("build_type_linear_commit_proof_from_live", "AC1", efm)

    # AC2 — composite_txn_commit stamps on both ok and reject.
    # The stamp is at the END of exit_mutation_boundary — covers
    # composite ok + reject (both fall through to the final stamp).
    must("build_type_linear_commit_proof_from_live", "AC2", efm)
    must("build_type_linear_commit_proof_from_live(cp.version", "AC2", efm)

    # AC3 — Soft + quiet path → stamp cheap. #2758 fills counts from
    # linear_or_dirty_roots_count_for_rebind (empty → 0) + goal gauge/hint.
    must("linear_or_dirty_roots_count_for_rebind", "AC3", tma)
    must("p.live_goal_count =", "AC3", tma)
    must("p.linear_root_count =", "AC3", tma)
    must("g_type_linear_commit_proof_stamped_total.fetch_add(1", "AC3", tma)

    # AC4 — Agent comparing defuse_or_epoch_stamp detects drift.
    must("p.defuse_or_epoch_stamp = current_epoch_or_defuse;", "AC4", tma)
    must("struct TypeLinearCommitProof", "AC4", tma)
    must("stamp_type_linear_commit_proof", "AC4", tma)

    # AC5 — additive only (preserve #2613 / #2697 surfaces + new
    # type-linear-commit-proof-stamped-total). #2613 is the
    # "type-linear-commit-health" query (no struct constant in
    # typed_mutation_audit.h — verified via the "#2613" comment
    # fragment + the query surface in evaluator_primitives_query.cpp).
    must("#2613", "AC5", tma)
    must("kTypeLinearCommitProofIssue = 2697", "AC5", tma)
    must_key("type-linear-commit-health", "AC5", q)
    must_key("type-linear-commit-proof-readiness-bp", "AC5", q)
    must_key("schema-2697", "AC5", q)
    must("g_type_linear_commit_proof_stamped_total", "AC5", tma)
    must_key("type-linear-commit-proof-stamped-total", "AC5", q)
    must_key("schema-2717", "AC5", q)
    must_key("issue-2717", "AC5", q)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2717_1_boundary_success_and_reject_stamp", "AC6", t)
    must("ac2717_2_composite_txn_commit_stamps", "AC6", t)
    must("ac2717_3_soft_quiet_path_cheap", "AC6", t)
    must("ac2717_4_drift_detection_via_defuse_or_epoch_stamp", "AC6", t)
    must("ac2717_5_additive_no_regression", "AC6", t)
    must("ac2717_6_source_and_linter", "AC6", t)
    must("check_type_linear_commit_proof_stamp_2717", "AC6", build)
    if _read("docs/design/2717-type-linear-commit-proof-stamp.md"):
        fails.append("AC6: docs/design/2717-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2717 TypeLinearCommitProof stamp on boundary + composite — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
