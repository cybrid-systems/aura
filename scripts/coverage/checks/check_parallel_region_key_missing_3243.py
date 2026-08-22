#!/usr/bin/env python3
"""Issue #3243: production multi-task mutate batches with missing/overlap
region_keys stay Serialized and expose a missing-keys signal.

Contract:
  AC1  Soft / single-task / pure: no counter bump, serialized-reason empty
  AC2  production + ≥2 tasks + distinct_nonzero < 2: isolation-level=serialized
       + serialized-reason + region_key_missing_serialized_total
  AC3  ≥2 distinct non-zero keys still RegionConcurrent (#2923/#2886)
  AC4  AURA_PARALLEL_REQUIRE_REGION_KEYS=1 optional deny (default off)
  AC5  extend test_parallel_intend_pure_contract; no invent / docs/design

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

    poh = _read("src/serve/parallel_orch.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")

    must("kParallelRegionKeyMissingIssue = 3243", "AC2 stamp", poh)
    must("region_key_missing_serialized_total", "AC2 counter", poh)
    must("region_key_missing_serialized(", "AC2 predicate", poh)
    must("missing-or-overlap-keys", "AC2 reason", poh)
    must("serialized-reason", "AC2 hash key", agent)
    must("region-key-missing-serialized", "AC2 batch flag", agent)
    must("production_defaults_active()", "AC1 production gate", agent)

    must("IsolationLevel::RegionConcurrent", "AC3 still RegionConcurrent", poh)
    must("ac3243_3_keys", "AC3 test", test)

    must("AURA_PARALLEL_REQUIRE_REGION_KEYS", "AC4 env", poh)
    must("parallel_require_region_keys_env", "AC4 helper", poh)
    must("ac3243_4_env_deny", "AC4 test", test)

    must("ac3243_1_soft", "AC1 Soft test", test)
    must("ac3243_2_prod_zero", "AC2 prod test", test)
    must("check_parallel_region_key_missing_3243", "AC5 build.py", build)
    if (ROOT / "tests" / "orch" / "test_issue_3243.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3243.cpp present (forbidden #81967)")
    if _read("docs/design/3243-region-key-missing.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3243 parallel_region_key_missing:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3243 parallel_region_key_missing: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
