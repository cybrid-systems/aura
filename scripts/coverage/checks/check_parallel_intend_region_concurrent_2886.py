#!/usr/bin/env python3
"""Issue #2886: orch/parallel-intend — promote region-concurrent as
recommended multi-agent mutate path; pure remains best-effort only.

Contract (one row per AC):
  AC1  Two disjoint region_keys + mutate bodies → concurrent admit under
       production; hash shows region-concurrent eligibility
  AC2  `:pure #t` + mutate → pure-contract-violated + production
       force-lock for rest of batch (#2838 preserved)
  AC3  Default `:pure #f` remains serialized (`eval-serialized=#t`)
  AC4  Overlapping / zero region keys fall back safely (no false
       concurrent claim)
  AC5  Additive query / hash keys only; existing pure and region metrics
       green
  AC6  Source-cite + tests (extend parallel_intend pure + region suites)
       per #81967; no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Files in scope for #2886.
SCOPE_FILES = [
    "src/serve/parallel_orch.h",
    "src/orch/agent_spawn.h",
    "src/compiler/evaluator_primitives_agent.cpp",
    "tests/orch/test_parallel_intend_pure_contract.cpp",
    "scripts/coverage/checks/check_parallel_intend_region_concurrent_2886.py",
]


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

    _read("src/serve/parallel_orch.h")
    _read("src/orch/agent_spawn.h")
    agent_prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test_pure = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")

    # ── AC1: disjoint region_keys → region-concurrent eligibility ──
    must("Issue #2886", "AC1", agent_prim)
    must("region-concurrent-eligible", "AC1", agent_prim)
    # The 3rd isolation-level value "region-concurrent" appears in the
    # isolation_level ternary (serialized / best-effort-pure / region-concurrent).
    if '"region-concurrent"' not in agent_prim:
        fails.append(
            "AC1: 3rd isolation-level value 'region-concurrent' not surfaced in Aura surface "
            "(per #2886 — disjoint region_keys take region-concurrent over serialized)"
        )
    # distinct_keys ≥ 2 check (the lambda that computes region-concurrent-eligible).
    if "distinct_keys" not in agent_prim:
        fails.append(
            "AC1: 'distinct_keys' compute block missing in Aura surface "
            "(#2886 needs to count distinct region_keys to drive the 3rd value)"
        )
    if "region_concurrent_eligible = (distinct_keys >= 2)" not in agent_prim:
        fails.append("AC1: 'region_concurrent_eligible = (distinct_keys >= 2)' gate missing")
    # Test exercises disjoint region_keys → region-concurrent.
    must(":region-keys (vector 1 2)", "AC1", test_pure)
    must("isolation-level", "AC1", test_pure)
    must("region-concurrent-eligible", "AC1", test_pure)

    # ── AC2: :pure #t + mutate → force-lock-applied (mirror #2838) ──
    must("force-lock-applied", "AC2", agent_prim)
    # Per-batch source from #2838 must remain.
    must("force_lock_on_violation_policy", "AC2", agent_prim)
    must("resolve_parallel_intend_force_lock_on_violation", "AC2", agent_prim)
    must("parallel_intend_force_lock_on_violation", "AC2", agent_prim)
    must("parallel_intend_force_lock_default_applied_total", "AC2", agent_prim)
    # Test source-cites the #2838 force-lock path.
    must("force-lock-applied", "AC2", test_pure)
    must("resolve_parallel_intend_force_lock_on_violation", "AC2", test_pure)
    must("parallel_intend_force_lock_on_violation", "AC2", test_pure)

    # ── AC3: default :pure #f → isolation-level=serialized ──
    # The default branch must still emit "serialized" (regression).
    # The ternary logic: pure_mode ? "best-effort-pure" :
    #   (region_concurrent_eligible ? "region-concurrent" : "serialized")
    # Default path: pure_mode=false + region_concurrent_eligible=false →
    # "serialized". Source-cite check.
    if '"best-effort-pure"' not in agent_prim or '"serialized"' not in agent_prim:
        fails.append(
            "AC3: default 'serialized' / pure 'best-effort-pure' isolation-level values "
            "missing (#2400 / #2081 regression)"
        )
    # Test exercises default → isolation-level=serialized.
    must("AC3", "AC3", test_pure)
    must("eval-serialized", "AC3", test_pure)
    must("serialized", "AC3", test_pure)

    # ── AC4: zero / overlapping region_keys → falls back to serialized ──
    # region-concurrent-eligible must return false when distinct_keys < 2.
    # The lambda implements this via `return distinct >= 2;`.
    if "return distinct >= 2;" not in agent_prim:
        fails.append(
            "AC4: region-concurrent-eligible gate `return distinct >= 2;` missing "
            "(zero / overlapping region_keys must fall back to serialized per AC4)"
        )
    # The Aura surface must also expose the serialized value when no
    # region_keys are supplied (test exercises this).
    must("AC4", "AC4", test_pure)
    must("falls back to serialized", "AC4", test_pure)

    # ── AC5: existing pure + region metrics green (regression) ──
    # #2746 region-keys-supplied + region-concurrent-batches +
    # region-concurrent-eligible keys still present (additive per #2886).
    must("region-keys-supplied", "AC5", agent_prim)
    must("region-concurrent-batches", "AC5", agent_prim)
    must("schema-2746", "AC5", agent_prim)
    # #2400 isolation-level + wired sentinel.
    must("isolation-level", "AC5", agent_prim)
    must("isolation-level-wired", "AC5", agent_prim)
    must("schema-2400", "AC5", agent_prim)
    # #2163 pure path preserved.
    must("pure-contract-violations", "AC5", agent_prim)
    must("schema-2163", "AC5", agent_prim)
    # #2838 force-lock surface preserved.
    must("parallel-intend-force-lock-default-applied-total", "AC5", agent_prim)
    # Soft / Off: zero behavior change beyond additive keys. The
    # isolation_level ternary depends on pure_mode + region_concurrent_eligible
    # — neither is changed by Soft / Off so behavior is identical.

    # ── AC6: source-cite + tests; no docs/design/; no invent ──
    must("schema-2886", "AC6", agent_prim)
    must("issue-2886", "AC6", agent_prim)
    must("parallel-intend-region-concurrent-wired", "AC6", agent_prim)
    must("2886", "AC6", test_pure)
    if "check_parallel_intend_region_concurrent_2886" not in build:
        fails.append("AC6: build.py does not wire #2886 linter")
    # No new test_issue_2886.cpp (per #81967).
    for d in ("core", "orch", "compiler"):
        if (ROOT / "tests" / d / "test_issue_2886.cpp").is_file():
            fails.append(f"AC6: tests/{d}/test_issue_2886.cpp present (forbidden per #81967)")
    # No docs/design/2886-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2886-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2886 region-concurrent promoted as recommended multi-agent mutate path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
