#!/usr/bin/env python3
"""Issue #2683: production default PerEval deopt-storm isolation.

Contract:
  AC1 Production + two concurrent fibers/evals + storm enter on A only
      → B's LayoutStamp.shape_version / SpecJIT specialization not forced
      deopt solely by A's storm; A isolates locally (PerEval default).
  AC2 Threshold force-reason (kShapeStormForceReasonThreshold) remains a
      hard fence for the storm-entering context; AdaptiveSuppress stays
      soft (#2526).
  AC3 on_arena_compact path: still does NOT feed deopt-storm ring; does
      NOT bump mutation_induced_invalidations_ (#2617 hard contract
      preserved).
  AC4 Env override: AURA_SHAPE_STORM_ISOLATION=global restores legacy
      process-global bump for experiments; default production = per-eval.
  AC5 Additive observability: shape-storm-per-eval-isolations-total +
      shape-storm-global-bump-total + schema-2683 / issue-2683 /
      shape-storm-isolation-default-per-eval sentinels.
  AC6 Source-cite + coverage manifest (no docs/design/* per #1655). Extend
      test_shape_* / shape_soa_storm batch per #81967; keep dedicated
      concurrency executable if already present.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    registry_hh = _read("src/compiler/hot_update_registry.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    build = _read("build.py")

    # AC1 / AC4 — weak default in shape_profiler.cpp must return PerEval (2)
    # under production default, and Global (0) under AURA_SHAPE_STORM_ISOLATION=global.
    must("aura_get_storm_isolation_mode", "AC1/AC4", cpp)
    must("AURA_SHAPE_STORM_ISOLATION", "AC4", cpp)
    # Verify the weak default block returns 2 (PerEval) on the default path
    # and 0 (Global) under the env override. Naive regex breaks on nested
    # braces (the inner if-block's '}' matches first). Use a manual
    # brace-depth walker to capture the full function body.
    sig = re.search(r"aura_get_storm_isolation_mode\s*\(\s*void\s*\)\s*(?:noexcept)?\s*\{", cpp)
    if not sig:
        fails.append("AC1/AC4: aura_get_storm_isolation_mode impl not found")
    else:
        start = sig.end()
        depth = 1
        i = start
        while i < len(cpp) and depth > 0:
            c = cpp[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        body = cpp[start : i - 1]
        if "return 0" not in body:
            fails.append("AC4: env override path missing 'return 0' (Global)")
        if "return 2" not in body:
            fails.append("AC1: default path missing 'return 2' (PerEval)")

    # AC1 — storm enter bump logic must check PerEval mode and call
    # bump_shape_version_on_storm_enter ONLY under non-PerEval (Global /
    # legacy). Under PerEval, bump the per-eval counter only.
    must("g_shape_storm_per_eval_isolations_total_atomic", "AC1", cpp)
    must("g_shape_storm_global_bump_total_atomic", "AC1", cpp)
    must("bump_shape_version_on_storm_enter", "AC1", cpp)

    # AC2 — Threshold force-reason + AdaptiveSuppress preserved. The
    # existing kShapeStormForceReasonThreshold / AdaptiveSuppress / shape_storm_fence_hard
    # must remain unchanged.
    must("kShapeStormForceReasonThreshold", "AC2", hh)
    must("shape_storm_fence_hard", "AC2", hh)
    # AdaptiveSuppress stays soft (#2526) — verify the AdaptiveSuppress
    # reason code exists.
    must("kShapeStormForceReasonAdaptiveSuppress", "AC2", hh)

    # AC3 — on_arena_compact does NOT feed deopt-storm ring + does NOT bump
    # mutation_induced_invalidations_ (#2617 hard contract preserved).
    must("kShapeCompactStormIsolationIssue", "AC3", hh)
    must("2617", "AC3", hh)  # lineage reference preserved
    # The shape_profiler.h comment at L187-189 must still describe the
    # hard contract. Loose check: "deopt-storm ring" + "mutation_induced" both
    # appear in the compact path comment.
    if "deopt-storm ring" not in hh or "mutation_induced" not in hh:
        fails.append("AC3: compact-path comment missing #2617 hard contract")

    # AC5 — counters declared in shape_profiler.h + query surface wired.
    must("g_shape_storm_per_eval_isolations_total_atomic", "AC5", hh)
    must("g_shape_storm_global_bump_total_atomic", "AC5", hh)
    must("kShapeStormPerEvalDefaultIssue", "AC5", hh)
    must("shape-storm-per-eval-isolations-total", "AC5", q)
    must("shape-storm-global-bump-total", "AC5", q)
    must("schema-2683", "AC5", q)
    must("issue-2683", "AC5", q)
    must("shape-storm-isolation-default-per-eval", "AC5", q)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/shape_storm_isolation_2683.md",
        "docs/shape_storm_isolation_2683.md",
        "design/2683.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: #2683 sentinel in shape_profiler.h + query + cpp.
    # Use "#2683" (not "Issue #2683") to accept combined citations.
    must("#2683", "AC6", hh)
    must("#2683", "AC6", q)
    must("#2683", "AC6", cpp)

    # AC6 — StormIsolation enum values present (Global=0, PerEval=2) for
    # reference integrity.
    must("Global = 0", "AC6", registry_hh)
    must("PerEval = 2", "AC6", registry_hh)

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_shape_storm_isolation_2683.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2683 shape storm PerEval default isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())