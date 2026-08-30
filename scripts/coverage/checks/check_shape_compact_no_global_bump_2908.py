#!/usr/bin/env python3
"""Issue #2908: ShapeProfiler PerEval harden — compact never bumps process-global shape_version.

Contract:
  AC1 Production PerEval + compact-only → process-global shape_version unchanged
  AC2 Mutation storm enter under PerEval → per-eval isolation, not global bump
  AC3 shape_compact_storm_isolation_wired + #2617 compact↛storm ring retained
  AC4 LayoutStamp force-reason Threshold hard; compact soft; query keys
  AC5 schema-2908 + linter; no docs/design/*; extend existing suite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_comments(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    return out


def _extract_fn_body(src: str, sig_pat: str) -> str | None:
    m = re.search(sig_pat, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    q = read_query_prims()
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_shape_compact_storm_isolation.cpp")
    build = _read("build.py")

    # AC1
    must("#2908", "AC1", hh)
    must("#2908", "AC1", cpp)
    must("kShapeCompactNoGlobalBumpIssue", "AC1", hh)
    must("g_shape_compact_global_version_skipped_total_atomic", "AC1", hh)
    must("allow_global_version_bump", "AC1", cpp)
    must("aura_get_storm_isolation_mode", "AC1", cpp)
    body = _extract_fn_body(cpp, r"ShapeProfiler::on_arena_compact\s*\(")
    if not body:
        fails.append("AC1: could not extract on_arena_compact body")
    else:
        stripped = _strip_comments(body)
        if "allow_global_version_bump" not in body:
            fails.append("AC1: compact body missing PerEval gate for global version")
        if "g_shape_compact_global_version_skipped_total_atomic" not in stripped:
            fails.append("AC1: compact must tally global-version-skipped under PerEval")
        # Unconditional shape_version_bump_count.fetch_add without gate is banned.
        # Accept only gated form inside if (allow_global_version_bump).
        if "if (allow_global_version_bump)" not in body and "if(allow_global_version_bump)" not in body:
            fails.append("AC1: shape_version_bump_count must be gated on allow_global_version_bump")

    # AC2
    must("g_shape_storm_per_eval_isolations_total_atomic", "AC2", cpp)
    must("g_shape_storm_global_bump_total_atomic", "AC2", cpp)
    must("iso_mode == 2", "AC2", cpp)
    must("#2908 AC2", "AC2", test)

    # AC3
    must("shape_compact_storm_isolation_wired", "AC3", hh)
    must("Explicitly do NOT call update_deopt_storm_state_", "AC3", cpp)
    must("check_shape_compact_storm_isolation_2617", "AC3", cpp)

    # AC4
    must("kShapeStormForceReasonThreshold", "AC4", hh)
    must("shape_storm_fence_hard", "AC4", hh)
    must("force-reason-threshold", "AC4", q)
    must("compact-global-version-skipped-total", "AC4", q)

    # AC5
    must("schema-2908", "AC5", q)
    must("schema-2908", "AC5", obs)
    must("compact-no-global-bump-wired", "AC5", q)
    must("check_shape_compact_no_global_bump_2908", "AC5", build)
    must("cmd_shape_compact_no_global_bump_2908", "AC5", build)
    must("#2908", "AC5", test)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2908-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2908.cpp").is_file():
        fails.append("tests/compiler/test_issue_2908.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2908 compact ↛ process-global shape_version — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
