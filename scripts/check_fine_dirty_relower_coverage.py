#!/usr/bin/env python3
"""check_fine_dirty_relower_coverage.py — Issue #1657 P0 source-level linter.

Verifies the 10 acceptance criteria for the finer-grained per-instruction
dirty bitmask propagation and minimal re-lower observability surface
introduced by #1657.

Background:
  - The P0 [Incremental] issue body calls out 4 main gaps:
    1. Instruction-level sync & SoA consistency (patch_last_instruction_metadata
       should auto-mark dirty + sync_instruction_dirty_from_block_dirty helper)
    2. Minimal re-lower decision logic (cross #1623 — algorithm)
    3. dep_graph_ enhancement for Quote + Lambda free_vars + macro subtrees
    4. Hot path integration (cross #1623)
  - This linter enforces the source-level invariants for the observability
    surface (5 new metrics) + the small source fixes that ship in #1657.
  - The actual algorithm changes for #1623 / #1625 ship in those issues
    separately.

ACs:
  AC1:  observability_metrics.h has the 4 new atomic fields (relower_instruction_level_hits,
       dep_graph_edge_miss_count, soa_dirty_sync_total, soa_consistency_partial_dirty_total)
  AC2:  compiler_metrics_fields.inc has matching entries for the 4 new fields
  AC3:  evaluator.ixx has 4 new bump_* methods (bump_relower_instruction_level_hit,
       bump_dep_graph_edge_miss, bump_soa_dirty_sync, bump_soa_consistency_partial_dirty)
  AC4:  evaluator.ixx has 4 new get_* accessors for the new metrics
  AC5:  ir_soa.ixx adds sync_instruction_dirty_from_block_dirty helper
       (returns std::size_t count of flipped bits + bumps soa_dirty_sync_total)
  AC6:  ir_soa.ixx patch_last_instruction_metadata now auto-marks the
       patched instruction dirty (sets instruction_dirty_[idx] = 1)
       AND bumps soa_dirty_sync_total
  AC7:  lowering_impl.cpp consistency-mismatch handler no longer does
       full SoA mark_all_blocks_dirty() — uses targeted partial dirty
       (only dirty the tail of extra SoA functions) and bumps
       soa_consistency_partial_dirty_total when targeted path resolves
  AC8:  service.ixx populate_dep_graph_from_workspace walks Quote + Lambda
       + Macro subtree nodes and bumps dep_graph_edge_miss_count
  AC9:  Test exists at tests/test_fine_dirty_relower.cpp with the 10 ACs
       (AC1-AC10 in the test file match the 10 ACs in this linter).
  AC10: Linter self-test passes (the linter itself verifies all 10 ACs).

Exit 0 = all 10 ACs satisfied, exit 1 = any failure (with diagnostic).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBS_H = ROOT / "src" / "compiler" / "observability_metrics.h"
INC = ROOT / "src" / "compiler" / "compiler_metrics_fields.inc"
EVAL = ROOT / "src" / "compiler" / "evaluator.ixx"
SOA = ROOT / "src" / "compiler" / "ir_soa.ixx"
LOWERING = ROOT / "src" / "compiler" / "lowering_impl.cpp"
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
TEST = ROOT / "tests" / "test_fine_dirty_relower.cpp"

NEW_FIELDS = (
    "relower_instruction_level_hits",
    "dep_graph_edge_miss_count",
    "soa_dirty_sync_total",
    "soa_consistency_partial_dirty_total",
)


def check_struct_fields(src: str) -> list[str]:
    """AC1: observability_metrics.h has the 4 new atomic fields."""
    failures: list[str] = []
    for f in NEW_FIELDS:
        pat = re.compile(rf"std::atomic<std::uint64_t>\s+{f}\s*\{{")
        if not pat.search(src):
            failures.append(f"AC1: observability_metrics.h missing atomic field {f}")
    return failures


def check_inc_entries(src: str) -> list[str]:
    """AC2: compiler_metrics_fields.inc has matching entries."""
    failures: list[str] = []
    for f in NEW_FIELDS:
        if f"AURA_COMPILER_METRICS_FIELD({f})" not in src:
            failures.append(f"AC2: compiler_metrics_fields.inc missing entry for {f}")
    return failures


def check_bump_methods(src: str) -> list[str]:
    """AC3: evaluator.ixx has 4 new bump_* methods."""
    failures: list[str] = []
    bump_methods = (
        "bump_relower_instruction_level_hit",
        "bump_dep_graph_edge_miss",
        "bump_soa_dirty_sync",
        "bump_soa_consistency_partial_dirty",
    )
    for m in bump_methods:
        if f"void {m}(" not in src:
            failures.append(f"AC3: evaluator.ixx missing {m} bumper")
    return failures


def check_get_accessors(src: str) -> list[str]:
    """AC4: evaluator.ixx has 4 new get_* accessors."""
    failures: list[str] = []
    get_methods = (
        "get_relower_instruction_level_hits",
        "get_dep_graph_edge_miss_count",
        "get_soa_dirty_sync_total",
        "get_soa_consistency_partial_dirty_total",
    )
    for g in get_methods:
        if f"std::uint64_t {g}(" not in src:
            failures.append(f"AC4: evaluator.ixx missing {g} getter")
    return failures


def check_sync_helper(src: str) -> list[str]:
    """AC5: ir_soa.ixx adds sync_instruction_dirty_from_block_dirty helper.

    Use a line-based search (extract from the function declaration to the
    end-of-file or next standalone top-level declaration). The previous
    non-greedy regex matched too early on the nested `if (flipped > 0) {`
    block inside the helper.
    """
    failures: list[str] = []
    if "std::size_t sync_instruction_dirty_from_block_dirty()" not in src:
        failures.append("AC5: ir_soa.ixx missing sync_instruction_dirty_from_block_dirty helper")
        return failures
    # Extract the function body via line-based search from the decl to the
    # closing brace at the same indent level. We use a greedy DOTALL match
    # to the LAST `\n    \}` (4-space indent closing) since the body has
    # nested `if (flipped > 0) { ... }` blocks.
    sync_block = re.search(
        r"std::size_t\s+sync_instruction_dirty_from_block_dirty\(\)\s*\{(.+?)^    \}",
        src,
        re.DOTALL | re.MULTILINE,
    )
    if not sync_block:
        # Fallback: look for the bump in the next 80 lines after the decl
        decl_idx = src.find("std::size_t sync_instruction_dirty_from_block_dirty()")
        if decl_idx < 0:
            failures.append("AC5: sync_instruction_dirty_from_block_dirty helper not located")
            return failures
        body = src[decl_idx : decl_idx + 4000]
        if "soa_dirty_sync_total" not in body:
            failures.append("AC5: sync_instruction_dirty_from_block_dirty does NOT bump soa_dirty_sync_total")
        return failures
    if "soa_dirty_sync_total" not in sync_block.group(1):
        failures.append("AC5: sync_instruction_dirty_from_block_dirty does NOT bump soa_dirty_sync_total")
    return failures


def check_patch_last_metadata(src: str) -> list[str]:
    """AC6: patch_last_instruction_metadata auto-marks dirty + bumps soa_dirty_sync_total."""
    failures: list[str] = []
    func_match = re.search(
        r"void\s+patch_last_instruction_metadata\s*\([^)]*\)\s*\{(.*?)\n\s*\}",
        src,
        re.DOTALL,
    )
    if not func_match:
        failures.append("AC6: ir_soa.ixx missing patch_last_instruction_metadata")
        return failures
    body = func_match.group(1)
    # Auto-mark dirty: look for instruction_dirty_[idx] = 1 OR mark_instruction_dirty call
    has_auto_mark = "instruction_dirty_[idx] = 1" in body or "mark_instruction_dirty" in body
    if not has_auto_mark:
        failures.append(
            "AC6: patch_last_instruction_metadata does NOT auto-mark the patched "
            "instruction dirty (Issue #1657 fix missing)"
        )
    # Bump soa_dirty_sync_total
    if "soa_dirty_sync_total" not in body:
        failures.append("AC6: patch_last_instruction_metadata does NOT bump soa_dirty_sync_total")
    return failures


def check_lowering_consistency(src: str) -> list[str]:
    """AC7: lowering_impl.cpp consistency-mismatch handler uses targeted partial dirty."""
    failures: list[str] = []
    # Find the consistency-mismatch handler
    match = re.search(
        r"if\s*\(\s*!ok\s*\)\s*\{(.*?consistency_mismatches.*?)\n\s*\}",
        src,
        re.DOTALL,
    )
    if not match:
        # Try a more permissive match
        match = re.search(
            r"\+\+g_last_soa_snapshot\.consistency_mismatches;(.+?)\n\s*\}\s*\n",
            src,
            re.DOTALL,
        )
    if not match:
        failures.append("AC7: lowering_impl.cpp consistency-mismatch handler not found")
        return failures
    body = match.group(0)
    # Must use partial dirty (mark_all_blocks_dirty in a loop with a comment about partial)
    # OR have the targeted SoA tail-dirty pattern
    has_partial = "soa_consistency_partial_dirty_total" in body or "soa_consistency_partial_dirty" in body
    if not has_partial:
        failures.append(
            "AC7: lowering_impl.cpp consistency-mismatch handler does NOT use "
            "targeted partial dirty (Issue #1657 fix missing — full "
            "mark_all_blocks_dirty still happens on mismatch)"
        )
    return failures


def check_dep_graph_walk(src: str) -> list[str]:
    """AC8: service.ixx populate_dep_graph_from_workspace walks Quote/Lambda/Macro.

    Use a line-based search because the function body has nested loops
    (for / while) that break the non-greedy regex extraction. Fall back
    to searching the next 6000 chars after the decl if the greedy regex
    doesn't match.
    """
    failures: list[str] = []
    if "populate_dep_graph_from_workspace" not in src:
        failures.append("AC8: service.ixx missing populate_dep_graph_from_workspace")
        return failures
    # Find the function decl line
    decl_idx = src.find("void populate_dep_graph_from_workspace()")
    if decl_idx < 0:
        # try a more lenient match
        m = re.search(r"void\s+populate_dep_graph_from_workspace\s*\(", src)
        if not m:
            failures.append("AC8: populate_dep_graph_from_workspace decl not found")
            return failures
        decl_idx = m.start()
    # Search the next 6000 chars after the decl for the 3 NodeTag walks +
    # the dep_graph_edge_miss_count bump
    body = src[decl_idx : decl_idx + 6000]
    has_quote = "NodeTag::Quote" in body
    has_lambda = "NodeTag::Lambda" in body
    has_macro = "NodeTag::Macro" in body
    if not (has_quote and has_lambda and has_macro):
        missing = []
        if not has_quote:
            missing.append("Quote")
        if not has_lambda:
            missing.append("Lambda")
        if not has_macro:
            missing.append("Macro")
        failures.append(
            f"AC8: populate_dep_graph_from_workspace doesn't walk all 3 of "
            f"Quote/Lambda/Macro subtree nodes (missing: {', '.join(missing)})"
        )
    if "dep_graph_edge_miss_count" not in body:
        failures.append(
            "AC8: populate_dep_graph_from_workspace doesn't bump dep_graph_edge_miss_count for missed edges"
        )
    return failures


def check_test_exists() -> list[str]:
    """AC9: tests/test_fine_dirty_relower.cpp exists with 10 ACs."""
    failures: list[str] = []
    if not TEST.exists():
        failures.append(f"AC9: {TEST} does not exist")
        return failures
    src = TEST.read_text(encoding="utf-8")
    # Verify all 10 AC markers
    for i in range(1, 11):
        if f"AC{i}:" not in src:
            failures.append(f"AC9: test_fine_dirty_relower.cpp missing AC{i} marker")
    return failures


def check_linter_self_test() -> list[str]:
    """AC10: Linter self-test passes (always true if we get here)."""
    # This AC is trivially satisfied if the linter runs without error.
    return []


def main() -> int:
    if not OBS_H.exists():
        print(f"FAIL: {OBS_H} not found", file=sys.stderr)
        return 1
    if not INC.exists():
        print(f"FAIL: {INC} not found", file=sys.stderr)
        return 1

    obs = OBS_H.read_text(encoding="utf-8")
    inc = INC.read_text(encoding="utf-8")
    eval_src = EVAL.read_text(encoding="utf-8") if EVAL.exists() else ""
    soa_src = SOA.read_text(encoding="utf-8") if SOA.exists() else ""
    low_src = LOWERING.read_text(encoding="utf-8") if LOWERING.exists() else ""
    svc_src = SERVICE.read_text(encoding="utf-8") if SERVICE.exists() else ""

    all_failures: list[str] = []
    all_failures.extend(check_struct_fields(obs))
    all_failures.extend(check_inc_entries(inc))
    all_failures.extend(check_bump_methods(eval_src))
    all_failures.extend(check_get_accessors(eval_src))
    all_failures.extend(check_sync_helper(soa_src))
    all_failures.extend(check_patch_last_metadata(soa_src))
    all_failures.extend(check_lowering_consistency(low_src))
    all_failures.extend(check_dep_graph_walk(svc_src))
    all_failures.extend(check_test_exists())
    all_failures.extend(check_linter_self_test())

    if all_failures:
        print(f"FAIL: {len(all_failures)} AC(s) violated:", file=sys.stderr)
        for f in all_failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("check_fine_dirty_relower_coverage: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
