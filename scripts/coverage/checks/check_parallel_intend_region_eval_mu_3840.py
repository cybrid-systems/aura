#!/usr/bin/env python3
"""Issue #3840: parallel-intend RegionConcurrent skips eval_mu under production.

Residual of #3728 — spawn skips agent_apply_mu_ under production + region
concurrency + non-zero region_key, but parallel-intend always took ash->eval_mu
when :pure #f even after decide_isolation stamped RegionConcurrent.

Contract (one row per AC):
  AC1  Production + workspace region concurrency + RegionConcurrent +
       per-task region_key != 0 → skip ash->eval_mu; eval-serialized=#f.
  AC2  Soft / Serialized / BestEffortPure / zero-key stay on eval_mu
       (eval-serialized=#t).
  AC3  Extends test_parallel_intend_pure_contract.cpp; linter + grandfather +
       manifest + build.py; no invent test_issue_3840.cpp / docs/design
       (#81967 / #1655). No second orch model.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

AGENT = "src/compiler/evaluator_primitives_agent.cpp"
TEST = "tests/orch/test_parallel_intend_pure_contract.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
LINTER = "check_parallel_intend_region_eval_mu_3840"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    agent = _read(AGENT)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)

    # Span the parallel-intend task lock site.
    # AC1 — RegionConcurrent skip gate mirrors spawn #3728.
    must("Issue #3840", "AC1 cite", agent)
    must("region_concurrent_skip_eval_mu", "AC1 skip gate", agent)
    must("region_unlocked_applies", "AC1 unlocked counter", agent)
    must("IsolationLevel::RegionConcurrent", "AC1 level gate", agent)
    must("workspace_region_concurrency_enabled()", "AC1 region concurrency", agent)
    must("production_defaults_active()", "AC1 production (spawn twin)", agent)
    # Per-task non-zero key still required.
    must("region_skip", "AC1 per-task region_skip", agent)
    must("rkey != 0", "AC1 zero-key stays locked", agent)
    # eval-serialized folds region unlock.
    must("region_unlocked_engaged", "AC1 eval-serialized fold", agent)
    must("schema-3840", "AC1 schema", agent)
    must("issue-3840", "AC1 issue stamp", agent)

    # AC2 — Soft / zero-key stay serialized (tests + code comments).
    must("Soft / Serialized / BestEffortPure / zero-key", "AC2 keep mutex comment", agent)
    must("ac3840_2_soft", "AC2 Soft test", test)
    must("ac3840_3_zero_key", "AC2/AC3 zero-key test", test)
    must("eval-serialized=#t", "AC2 Soft serialized assert text", test)

    # AC1 fixture.
    must("ac3840_1_prod_keys", "AC1 prod keys test", test)
    must("eval-serialized=#f", "AC1 unlocked assert text", test)
    must("set_workspace_region_concurrency_enabled(true)", "AC1 enables region concurrency", test)
    must("3840 AC1", "AC1 fixture label", test)
    must("3840 AC2", "AC2 fixture label", test)
    must("3840 AC3", "AC3 fixture label", test)
    must("3840 AC4", "AC4 fixture label", test)

    # AC3 — wiring + no invent.
    must(LINTER, "AC3 build registration", build)
    must("Issue #3840", "AC3 build cite", build)
    must("check_parallel_intend_region_eval_mu_3840.py", "AC3 grandfather basename", gf)
    must(
        "scripts/coverage/checks/check_parallel_intend_region_eval_mu_3840.py",
        "AC3 grandfather path",
        gf,
    )
    must_not("query:3840", "AC3 no new query", agent)
    if (ROOT / "tests" / "orch" / "test_issue_3840.cpp").is_file():
        fails.append("AC3: tests/orch/test_issue_3840.cpp present (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3840.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3840.cpp present (forbidden)")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        bad = [p.name for p in design.iterdir() if p.name.startswith("3840")]
        if bad:
            fails.append(f"AC3: docs/design/ has {bad}")
    # Manifest present.
    if not (ROOT / "scripts" / "coverage" / "manifests" / "3840.json").is_file():
        fails.append("AC3: scripts/coverage/manifests/3840.json missing")

    if fails:
        print(f"check_parallel_intend_region_eval_mu_3840: {len(fails)} row(s) failed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3840 parallel-intend RegionConcurrent skips eval_mu under production")
    return 0


if __name__ == "__main__":
    sys.exit(main())
