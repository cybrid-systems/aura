#!/usr/bin/env python3
"""Issue #3353: production_defaults multi-agent mutate missing keys deny.

#3299 left production mutate batches Serialized unless the host set
AURA_PARALLEL_REQUIRE_REGION_KEYS=1. #3353 makes production_defaults
fail-closed for mutate batches with <2 distinct non-zero region_keys.

Contract:
  AC1 production_defaults + mutate + missing keys → structured deny
  AC2 ≥2 distinct keys still RegionConcurrent
  AC3 Soft / Off → Serialized; env=0 soak escape
  AC4 after #3299; overlap deny; #3243 non-mutate Serialized retained
  AC5 no invent / docs/design / schema-3353 / g_3353_* / new mutate:* key

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
    md = _read("src/orch/README.md")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp") + _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    must("kParallelMutateRegionKeysProductionDenyIssue = 3353", "AC1 stamp", poh)
    must("parallel_require_region_keys_deny", "AC1 deny helper", poh)
    must("parallel_require_region_keys_explicit_off", "AC1 env=0 escape", poh)
    must("/*mutate_batch=*/true", "AC1 !pure mutate-capable deny", agent)
    must("ac3353_1_prod_mutate_deny", "AC1 test", test)
    must("production && mutate_batch", "AC1 production gate", poh)

    must("ac3353_2_keys", "AC2 RegionConcurrent", test)
    must("IsolationLevel::RegionConcurrent", "AC2 still RegionConcurrent", poh)

    must("ac3353_3_soft", "AC3 Soft", test)
    must("ac3353_4_env_off", "AC3 env=0", test)
    must("parallel_require_region_keys_explicit_off()", "AC3 skip on 0", poh)

    must("check_parallel_mutate_region_keys_prod_deny_3353", "AC4 build.py", build)
    must("check_parallel_mutate_region_keys_3299", "AC4 after #3299", build)
    i3299 = build.find("check_parallel_mutate_region_keys_3299.py")
    i3353 = build.find("check_parallel_mutate_region_keys_prod_deny_3353.py")
    if i3299 < 0 or i3353 < 0 or i3353 < i3299:
        fails.append("AC4: #3353 linter must run after #3299")
    must("kMutateOverlapKeys", "AC4 overlap snippet", test)
    must("ac3353_4_env_off", "AC4 env=0", test)
    must("structured-denied", "AC4 README", md)

    must("ac3353_1_prod_mutate_deny", "AC5 test", test)
    if "schema-3353" in agent or "schema-3353" in poh or "schema-3353" in q:
        fails.append("AC5: new schema-3353 query key")
    if "g_3353_" in agent or "g_3353_" in poh:
        fails.append("AC5: new g_3353_* counter")
    if (ROOT / "tests" / "orch" / "test_issue_3353.cpp").is_file():
        fails.append("AC5: forbidden tests/orch/test_issue_3353.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3353.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3353.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3353-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3353 parallel_mutate_region_keys_prod_deny:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3353 parallel_mutate_region_keys_prod_deny: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
