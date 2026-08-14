#!/usr/bin/env python3
"""Issue #3017: densify incomplete-remap residual (value-only / un-slotted).

Contract (one row per AC):
  AC1  Audit intermediate / FFI / agent / scratch create paths: any
       allocate that bypasses create / wire / slot / EXEMPT fails.
       register_external_root_for_densify (value-only) is NOT cover.
  AC2  Production hard: uncovered external → pre-move reject + sticky-off.
       Clean densify after slot/pin cover may clear sticky (#2905).
  AC3  Soft / Off / hard_pref<=0 stays a single atomic load (zero extra
       walk, no value-only-not-cover bump).
  AC4  Additive schema-3017 on densify-health (value-only-not-cover-total
       + incomplete-remap-residual-wired). Lineage 2495/2837/2973 kept.
  AC5  mutate × densify soak + untracked inject canary in
       test_moving_densify_fail_closed (#81967). No test_issue_3017.cpp;
       no docs/design/ (#1655). No new pin registry.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Intermediate / FFI / agent / scratch create surfaces (#3017 AC1).
# Extends #2709's four-file set so a new allocate in agent/FFI/scratch
# cannot bypass the create/wire/slot/EXEMPT triad.
SCAN_FILES = [
    "src/compiler/evaluator_primitives_eval.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/compiler/evaluator_eval_flat.cpp",
    "src/compiler/evaluator_primitives_agent.cpp",
    "src/compiler/evaluator_primitives_test.cpp",
    "src/compiler/evaluator_primitives_memory.cpp",
    "src/compiler/ffi_primitives_impl.cpp",
    "src/compiler/ffi_hot_path.hh",
    "src/compiler/ffi_hot_path.ixx",
]

# Bare allocate that is NOT ASTArena::create (the create leg of the triad).
_ALLOCATE_BYPASS = re.compile(r"\b(try_allocate|allocate_raw|allocate_checked)\s*\(")
_VALUE_ONLY = re.compile(r"\bregister_external_root_for_densify\s*\(")
_SAFE_COVER = re.compile(
    r"\b(wire_general_object_create_pair_or_required_fail|"
    r"wire_general_object_create_pair_or_exempt|"
    r"wire_general_object_create_pair|"
    r"register_external_root_slot_for_densify|"
    r"GENERAL_OBJECT_PIN_EXEMPT)\s*\("
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _value_only_is_not_cover(body: str) -> bool:
    """True when value-only prep appears without slot / wire / EXEMPT."""
    if _VALUE_ONLY.search(body) is None:
        return False
    return _SAFE_COVER.search(body) is None


def _scan_allocate_bypass(files: list[str]) -> list[str]:
    """Fail sites: try_allocate / allocate_raw / allocate_checked without
    wire / slot / EXEMPT in the enclosing function. `.create<` is the
    create leg of the triad (ASTArena::create auto-wires under required)
    and is not a bypass.
    """
    fails: list[str] = []
    for rel in files:
        body = _read(rel)
        if not body:
            # Optional FFI headers may be thin re-exports.
            if rel.endswith((".hh", ".ixx")):
                continue
            fails.append(f"{rel}: missing scan file")
            continue
        for i, line in enumerate(body.split("\n"), start=1):
            if _ALLOCATE_BYPASS.search(line) is None:
                continue
            # Window: nearby cover is enough (function-level 2709 is too
            # coarse for eval_flat's huge bodies).
            lines = body.split("\n")
            lo = max(0, i - 1 - 40)
            hi = min(len(lines), i - 1 + 40)
            window = "\n".join(lines[lo:hi])
            if _SAFE_COVER.search(window) is None:
                fails.append(f"{rel}:{i}: allocate bypass without wire/slot/EXEMPT")
    return fails


def _scan_value_only_as_cover(files: list[str]) -> list[str]:
    fails: list[str] = []
    for rel in files:
        body = _read(rel)
        if not body:
            continue
        if _value_only_is_not_cover(body):
            fails.append(f"{rel}: value-only register_external_root_for_densify is not cover")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    arena = _read("src/core/arena.ixx")
    report = _read("src/core/densify_consistency_report.h")
    lp = _read("src/core/lifetime_pin.hh")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1: caller audit + value-only is not cover ──
    must("Issue #3017: value-only prep is observability only, not safe cover", "AC1 arena", arena)
    must("value-only prep is observability only", "AC1 count helper", arena)
    must("value-only register_external_root_for_densify is not the", "AC1 lifetime_pin", lp)
    for rel in SCAN_FILES:
        if rel.endswith((".hh", ".ixx")) and not (ROOT / rel).is_file():
            continue
        if not (ROOT / rel).is_file():
            fails.append(f"AC1: missing scan file {rel}")
    bypass = _scan_allocate_bypass(SCAN_FILES)
    for b in bypass:
        fails.append(f"AC1: {b}")
    voc = _scan_value_only_as_cover(SCAN_FILES)
    for v in voc:
        fails.append(f"AC1: {v}")
    # Synthetic: value-only alone must be classified as not cover.
    if not _value_only_is_not_cover("arena.register_external_root_for_densify(ext);"):
        fails.append("AC1: linter must treat value-only prep as not cover")
    if _value_only_is_not_cover(
        "arena.register_external_root_for_densify(ext);\narena.register_external_root_slot_for_densify(&ext);"
    ):
        fails.append("AC1: slot register must count as safe cover")

    # ── AC2: production hard pre-move reject + sticky-off ──
    must("Issue #2973 / #3017", "AC2 pre-move cite", arena)
    must("Value-only", "AC2 value-only not cover in gate", arena)
    must("g_moving_value_only_not_cover_total.fetch_add", "AC2 counter bump", arena)
    must("g_moving_incomplete_remap_sticky_densify_off.exchange", "AC2 sticky", arena)
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC2 sticky clear", arena)
    gate_pos = arena.find("count_pre_densify_untracked_external_roots_()")
    reloc_pos = arena.find("result.objects_moved = relocate_tracked_objects_for_moving_")
    if gate_pos == -1 or reloc_pos == -1 or gate_pos > reloc_pos:
        fails.append("AC2: pre-check must precede relocate_tracked_objects_for_moving_ call")

    # ── AC3: Soft / Off zero cost ──
    must("hard_pref<=0 is a single atomic load", "AC3 Soft skip", arena)
    must("ac3017_5_soft_zero_cost", "AC3 test", test)

    # ── AC4: additive schema ──
    must("g_moving_value_only_not_cover_total", "AC4", report)
    must("kMovingIncompleteRemapResidual3017Issue = 3017", "AC4", report)
    must("reset_moving_incomplete_remap_residual_3017_for_test", "AC4", report)
    must("value-only-not-cover-total", "AC4 densify-health", obs)
    must("incomplete-remap-residual-wired", "AC4 densify-health", obs)
    must("schema-3017", "AC4 densify-health", obs)
    must("issue-3017", "AC4 densify-health", obs)
    must("schema-2973", "AC4 lineage 2973", obs)
    must("schema-2495", "AC4 lineage 2495", obs)

    # ── AC5: tests + linter + no invent / no design / no new pin registry ──
    must("ac3017_1_audit_value_only_not_cover", "AC5", test)
    must("ac3017_2_untracked_inject_canary", "AC5", test)
    must("ac3017_3_mutate_densify_soak", "AC5", test)
    must("ac3017_4_sticky_recover_after_inject", "AC5", test)
    must("ac3017_5_soft_zero_cost", "AC5", test)
    must("ac3017_6_linter_and_no_design", "AC5", test)
    must("check_moving_incomplete_remap_3017", "AC5 build", build)
    must("cmd_moving_incomplete_remap_3017", "AC5 build cmd", build)
    if "new pin registry" in arena.lower() and "do not" not in arena.lower():
        pass
    # No second pin registry symbol.
    if "g_moving_pin_registry_3017" in arena or "class DensifyPinRegistry" in arena:
        fails.append("AC5: must not introduce a new pin registry")
    design_docs = sorted((ROOT / "docs" / "design").glob("3017-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC5: docs/design/3017-* present ({[p.name for p in design_docs]})")
    if (ROOT / "tests" / "core" / "test_issue_3017.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3017.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_moving_incomplete_remap_3017: {len(fails)} failure(s)")
        return 1
    print("check_moving_incomplete_remap_3017: OK (AC1-AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
