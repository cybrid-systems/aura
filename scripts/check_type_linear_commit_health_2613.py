#!/usr/bin/env python3
"""Issue #2613: unified query:type-linear-commit-health coverage.

Contract:
  AC1 pure compute_type_linear_commit_health + query registration + schema-2613
  AC2 force_reason folds blame/linear/coercion-slo; codes 2/3/7
  AC3 happy path readiness 10000 / ok
  AC4 no commit policy change; detailed queries retained
  AC5 test + cmake + build.py gate; no design docs

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

    hh = _read("src/compiler/type_linear_commit_health.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_type_linear_commit_health_2613.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2613", "AC1", hh)
    must("compute_type_linear_commit_health", "AC1", hh)
    must("query:type-linear-commit-health", "AC1", q)
    must("schema-2613", "AC1", q)
    must("query:type-linear-commit-health", "AC1", obs)
    must("ac1_query_keys", "AC1", test)

    # AC2
    must("coercion-slo", "AC2", hh)
    must("occurrence-stale", "AC2", hh)
    must("force_reason_code", "AC2", hh)
    must("ac2_force_reason_match", "AC2", test)

    # AC3
    must("readiness_bp", "AC3", hh)
    must("would_allow_commit", "AC3", hh)
    must("ac3_happy_path", "AC3", test)

    # AC4
    must("commit_readiness", "AC4", hh)
    must("no commit barrier", "AC4", hh)
    must("ac4_pure_no_policy", "AC4", test)
    must("type-incremental-fidelity-stats", "AC4", test)

    # AC5
    must("test_type_linear_commit_health_2613", "AC5", cmake)
    must("check_type_linear_commit_health_2613", "AC5", build)
    must("cmd_type_linear_commit_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)
    for rel in (
        "docs/design/type_linear_commit_health_2613.md",
        "docs/type_linear_commit_health_2613.md",
        "design/2613.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2613 type-linear-commit-health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
