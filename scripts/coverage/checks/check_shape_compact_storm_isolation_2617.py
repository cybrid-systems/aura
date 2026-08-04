#!/usr/bin/env python3
"""Issue #2617: compact path must never feed deopt-storm ring as mutation.

Contract:
  AC1 Gate: on_arena_compact body never calls update_deopt_storm_state_
  AC2 Pure-compact stress: Threshold force-reason stays none; compact counters advance
  AC3 Mutation invalidate still trips storm logic
  AC4 on_arena_compact preserves is_stable + history (no thrash)
  AC5 Source-cite #1521/#2257/#2526/#2617 + schema-2617; no design docs

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


def _strip_comments_and_strings(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def _extract_fn_body(src: str, sig_pat: str) -> str | None:
    """Return body text of the first function matching sig_pat (brace-balanced)."""
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

    sph = _read("src/compiler/shape_profiler.h")
    spc = _read("src/compiler/shape_profiler.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_shape_compact_storm_isolation.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # ── AC1: on_arena_compact must not call update_deopt_storm_state_ ──
    must("COMPACT", "AC1", spc)  # banner comment fragment
    must("check_shape_compact_storm_isolation_2617", "AC1", spc)
    must("kShapeCompactStormIsolationIssue", "AC1", sph)
    must("Explicitly do NOT call update_deopt_storm_state_", "AC1", spc)

    compact_body = _extract_fn_body(spc, r"ShapeProfiler::on_arena_compact\s*\(\s*\)\s*noexcept")
    if not compact_body:
        fails.append("AC1: could not extract on_arena_compact body")
    else:
        stripped = _strip_comments_and_strings(compact_body)
        if re.search(r"\bupdate_deopt_storm_state_\s*\(", stripped):
            fails.append("AC1: on_arena_compact calls update_deopt_storm_state_ (forbidden)")
        if "deopt_storm_compact_suppressed" not in compact_body:
            fails.append("AC1: on_arena_compact must tally deopt_storm_compact_suppressed")
        # Compact must not fetch_add mutation counters (loads for contract ok).
        if re.search(r"mutation_induced_invalidations_\.fetch_add", stripped):
            fails.append("AC1: on_arena_compact must not bump mutation_induced_invalidations_")

    # Call sites of update_deopt_storm_state_ must be mutation/stability only
    stripped_all = _strip_comments_and_strings(spc)
    call_sites = [m.start() for m in re.finditer(r"\bupdate_deopt_storm_state_\s*\(", stripped_all)]
    # One is the definition; others are calls
    def_m = re.search(
        r"void\s+ShapeProfiler::update_deopt_storm_state_\s*\(",
        stripped_all,
    )
    real_calls = []
    for pos in call_sites:
        if def_m and def_m.start() <= pos < def_m.end():
            continue
        # Skip if this is the definition signature
        window = stripped_all[max(0, pos - 40) : pos + 40]
        if "ShapeProfiler::update_deopt_storm_state_" in window and "void" in window:
            continue
        real_calls.append(pos)
    if len(real_calls) < 1:
        fails.append("AC1: expected mutation-path calls to update_deopt_storm_state_")
    # Both expected callers: invalidate_unlocked_ and record_shape stability loss
    must("invalidate_unlocked_", "AC1", spc)
    must("ac1_gate_compact_no_storm_ring", "AC1", test)

    # ── AC2 pure-compact stress (source + test wiring) ──
    must("ac2_pure_compact_no_threshold", "AC2", test)
    must("deopt_storm_total", "AC2", test)
    must("kShapeStormForceReasonThreshold", "AC2", sph)

    # ── AC3 mutation still storms ──
    must("ac3_mutation_still_storms", "AC3", test)
    must("update_deopt_storm_state_", "AC3", spc)

    # ── AC4 stable + history preserved ──
    must("ac4_stable_history_preserved", "AC4", test)
    must("is_stable", "AC4", spc)
    must("arena_compact_stable_preserved", "AC4", spc)

    # ── AC5 source-cite + schema ──
    must("#2617", "AC5", sph)
    must("#2617", "AC5", spc)
    must("#1521", "AC5", sph)
    must("schema-2617", "AC5", q)
    must("compact-storm-isolated-wired", "AC5", q)
    must("deopt-storm-compact-suppressed", "AC5", q)
    must("shape_compact_storm_isolation_wired", "AC5", sph)
    must("test_shape_compact_storm_isolation", "AC5", cmake)
    must("check_shape_compact_storm_isolation_2617", "AC5", build)
    must("cmd_shape_compact_storm_isolation_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    # Retain adaptive suppress lineage (#2526) — soft, not Threshold
    must("kShapeStormForceReasonAdaptiveSuppress", "retain", sph)
    must("adaptive_thr", "retain", spc)

    for rel in (
        "docs/design/shape_compact_storm_isolation_2617.md",
        "docs/shape_compact_storm_isolation_2617.md",
        "design/2617.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2617 compact↛storm isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
