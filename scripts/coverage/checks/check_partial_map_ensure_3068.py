#!/usr/bin/env python3
"""Issue #3068: partial relower must ensure + snapshot impact_ub before decision.

Workspace `relower_dirty_defines_from_workspace` recovers / completes
`source_to_ir_map` in the same critical section as the threshold consult.
Soft/clean: no extra rebuild when the map is already consistent.

Contract:
  AC1 Injected map desync → recover or force-full before peel
  AC2 Missing CastOp / instr loc must not silent-partial
  AC3 Existing counters only (no new middle-layer)
  AC4 extend test_incremental_relower_batch + map recover + cascade skip;
      soak/linter; no docs/design/; no test_issue_3068

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    svc = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    pure = _read("src/compiler/ir_cache_pure.ixx")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    batch = _read("tests/compiler/test_incremental_relower_batch.cpp")
    rec = _read("tests/compiler/test_source_to_ir_desync_recovery.cpp")
    skip = _read("tests/compiler/test_cascade_relower_silent_skip.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3068", "AC1 service", svc)
    must("prepare_source_to_ir_map_for_partial_", "AC1 prepare", svc)
    must("ensure_source_to_ir_map_", "AC1 ensure", svc)
    must("recover_source_to_ir_map_desync", "AC1 recover", svc)
    must("ac3068_1_inject_desync", "AC1 batch test", batch)
    must("inject_source_to_ir_map_desync_for_test", "AC1 inject", batch)

    # AC2
    must("source_to_ir_map_missing_instr_loc", "AC2 pure helper", pure)
    must("Issue #3068", "AC2 pure cite", pure)
    must("inject_source_to_ir_map_drop_instr_loc_for_test", "AC2 drop hook", svc)
    must("CastOp", "AC2 CastOp prefer", svc)
    must("ac3068_2_missing_instr_loc", "AC2 batch test", batch)
    must("run_3068", "AC2 batch runner", batch)

    # AC3
    must("partial_forced_full_by_impact_total", "AC3 existing impact", svc)
    must("source_to_ir_map_rebuild_total", "AC3 existing rebuild", svc)
    must("schema-3068", "AC3 query", q)
    must("issue-3068", "AC3 issue key", q)
    must("map-ensure-before-partial-wired", "AC3 wired", q)
    if False:
        pass
    # No new CompilerMetrics field for 3068.
    met = _read("src/compiler/observability_metrics.h")
    if "partial_map_ensure" in met or "map_ensure_before_partial_total" in met:
        fails.append("AC3: new middle-layer counter in observability_metrics.h")

    # AC4
    must("Issue #3068", "AC4 dirty", dirty)
    must("prepare_source_to_ir_map_for_partial_", "AC4 dirty prepare", dirty)
    must("ac3068_relower_recovers_or_full", "AC4 recover suite", rec)
    must("inject_source_to_ir_map_drop_instr_loc_for_test", "AC4 recover AC2", rec)
    must("Issue #3068", "AC4 cascade skip", skip)
    must("check_partial_map_ensure_3068", "AC4 build", build)
    must("cmd_partial_map_ensure_3068", "AC4 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3068.cpp").is_file():
        fails.append("AC4: test_issue_3068.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3068*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3068 map ensure + impact_ub before partial decision — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
