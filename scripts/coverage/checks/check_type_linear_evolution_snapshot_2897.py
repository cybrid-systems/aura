#!/usr/bin/env python3
"""Issue #2897: query:type-linear-evolution-snapshot coverage.

Contract:
  AC1 primitive registered; keys + schema-2897
  AC2 values match SSOT gauges (capture helper pure fold)
  AC3 Soft quiet path zeros / no CS walk
  AC4 additive; preserve #2613/#2697/#2842/#2854/#2860
  AC5 source-cite + extend test_type_linear_commit_health; no docs/design/
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/type_linear_commit_health.hh")
    q = read_query_prims()
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("2897", "AC1", hh)
    must("capture_type_linear_evolution_snapshot", "AC1", hh)
    must("TypeLinearEvolutionSnapshot", "AC1", hh)
    must("query:type-linear-evolution-snapshot", "AC1", q)
    must("schema-2897", "AC1", q)
    must("last-proof-outcome", "AC1", q)
    must("live-goal-count", "AC1", q)
    must("goal-fingerprint", "AC1", q)
    must("linear-root-count", "AC1", q)
    must("partial-cone-truncated", "AC1", q)
    must("occurrence-empty-after-fence", "AC1", q)
    must("cone-outside-goal-drop", "AC1", q)
    must("type-linear-evolution-snapshot-wired", "AC1", q)
    must("query:type-linear-evolution-snapshot", "AC1", obs)

    must("last_proof_live_goal_count_v_read", "AC2", hh)
    must("last_type_linear_proof_outcome_v_read", "AC2", hh)
    must("commit_readiness_live_policy", "AC2", hh)

    must("no CS walk", "AC3", hh)
    must("schema-2613", "AC4", q)
    must("schema-2697", "AC4", q)
    must("schema-2842", "AC4", q)
    must("schema-2854", "AC4", q)
    must("schema-2860", "AC4", q)
    must("query:type-linear-commit-health", "AC4", q)
    must("query:evolution-epoch-snapshot", "AC4", q)

    must("ac2897_1_query_keys", "AC5", t)
    must("ac2897_2_matches_ssot", "AC5", t)
    must("ac2897_3_quiet_cheap", "AC5", t)
    must("ac2897_4_additive", "AC5", t)
    must("ac2897_5_source_cite", "AC5", t)
    must("check_type_linear_evolution_snapshot_2897", "AC5", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2897-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2897.cpp").is_file():
        fails.append("tests/compiler/test_issue_2897.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2897 type-linear-evolution-snapshot — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
