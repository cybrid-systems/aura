#!/usr/bin/env python3
"""Issue #2553: single Agent commit-readiness score.

Contract:
  AC1 CommitReadiness + commit_readiness pure helper
  AC2 empty_cs / truncate / linear / blame / solve priority
  AC3 Soft vs hard policy bands
  AC4 query schema-2553 + keys on fidelity-stats
  AC5 test + cmake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    th = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_commit_readiness_score_2553.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2553", "AC1", th)
    must("struct CommitReadiness", "AC1", th)
    must("struct CommitReadinessInput", "AC1", th)
    must("commit_readiness(const CommitReadinessInput", "AC1", th)
    must("ac1_clean_ok", "AC1", test)

    # AC2 priority
    must("empty_cs", "AC2", th)
    must("truncate", "AC2", th)
    must("force_reason priority", "AC2", th)
    must("ac2_empty_cs_hard", "AC2", test)
    must("ac3_truncate_hard", "AC2", test)

    # AC3 soft
    must("Soft/Sampled", "AC3", th)
    must("ac4_soft_observe", "AC3", test)

    # AC4 query
    must("schema-2553", "AC4", q)
    must("commit-readiness-bp", "AC4", q)
    must("commit-readiness-would-allow", "AC4", q)
    must("commit-readiness-force-reason", "AC4", q)
    must("commit-readiness-wired", "AC4", q)
    must("commit_readiness_live_policy", "AC4", th)

    # AC5 gate
    must("test_commit_readiness_score_2553", "AC5", cmake)
    must("check_commit_readiness_score_2553", "AC5", build)
    must("cmd_commit_readiness_score_coverage", "AC5", build)
    must("ac5_source_schema_live", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2553 commit-readiness score — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
