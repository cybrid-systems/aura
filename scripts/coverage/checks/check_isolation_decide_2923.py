#!/usr/bin/env python3
"""Issue #2923: authoritative IsolationLevel decide_isolation API.

AC:
  1. IsolationLevel + decide_isolation in parallel_orch.h (pure, header-only)
  2. pure_mode → BestEffortPure; ≥2 keys → RegionConcurrent; else Serialized
  3. Aura orch:parallel-intend derives isolation-level from decide_isolation only
  4. No second ternary in agent primitives
  5. Extend test_parallel_intend_pure_contract (no test_issue_N); build.py wire
  6. No docs/design/2923-* per #1655
"""

from __future__ import annotations

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    poh = _read("src/serve/parallel_orch.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")

    must("Issue #2923" in poh, "AC1: parallel_orch cites #2923")
    must("enum class IsolationLevel" in poh, "AC1: IsolationLevel enum")
    must("IsolationDecision" in poh, "AC1: IsolationDecision")
    must("decide_isolation" in poh, "AC1: decide_isolation")
    must("isolation_level_cstr" in poh, "AC1: isolation_level_cstr")
    must("RegionConcurrent" in poh and "BestEffortPure" in poh and "Serialized" in poh, "AC1: three isolation levels")
    must("count_distinct_nonzero_region_keys" in poh, "AC1: distinct key helper")

    must("decide_isolation" in agent, "AC3: Aura calls decide_isolation")
    must("isolation_level_cstr" in agent, "AC3: Aura uses isolation_level_cstr")
    must('pure_mode ? "best-effort-pure"' not in agent, "AC4: no second pure ternary")
    must(
        'region_concurrent_eligible ? "region-concurrent"' not in agent,
        "AC4: no second region ternary",
    )
    # region-concurrent-eligible must share decision (no independent recount lambda)
    must(
        "make_bool(region_concurrent_eligible)" in agent
        or "make_bool(iso_decision.region_concurrent_eligible)" in agent,
        "AC3: region-concurrent-eligible from decision",
    )

    must("2923" in test and "decide_isolation" in test, "AC5: contract test extended")
    must("#2923 AC1" in test, "AC5: AC1 row in test")
    must("isolation-decide-2923" in build or "isolation_decide_2923" in build, "AC5: build.py")

    must(not (ROOT / "tests/orch/test_issue_2923.cpp").is_file(), "AC5: no invent test_issue_2923")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2923-*"):
            fails.append(f"AC6: docs/design/{f.name} forbidden per #1655")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2923 IsolationLevel decide_isolation SSOT — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
