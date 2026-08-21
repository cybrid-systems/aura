#!/usr/bin/env python3
"""Issue #3214: required-regime intermediate allocate must cover triad on
all densify-tracked paths (not only small-pool). Residual of
#2971/#3093/#3156/#3180.

Background
----------
`ASTArena::maybe_note_allocate_intermediate_` (#3053 + #3156 + #3180) gated
the cover triad on `size <= kMaxSmallSize && small_pool_.owns(ptr)`, and
`allocate_raw_impl` only called it on the small-pool hit path. Larger
objects, pmr fallback, and non-create allocate paths that can enter
`last_object_remap_` skipped inventory — under production required this
bypasses the pin contract.

`#3214` closes the residual by:

  1. Broadening `maybe_note_allocate_intermediate_` so non-small /
     `!owns(ptr)` still routes through `note_intermediate_create_with_cover_`
     (same triad; no second registry). Soft still returns on the first
     `general_object_pin_required_active()` load.
  2. Calling `maybe_note_allocate_intermediate_` on the pmr /
     small-pool-fallback path in `allocate_raw_impl` (not only the
     small-pool hit).
  3. Extending `test_moving_densify_fail_closed`,
     `test_arena_required_cover_no_value_only`, and
     `test_general_object_pin_coverage_gate` (#81967).

This linter is the regression guard:

  * AC1_NOTE_ALL_SIZES — maybe_note keeps the required-active load +
    render-hotpath gate + kMaxSmallSize / owns identity, but MUST NOT
    early-return on `size > kMaxSmallSize || !owns` without noting.
    Non-small branch still calls `note_intermediate_create_with_cover_`.
  * AC2_PMR_PATH_NOTES — `allocate_raw_impl` calls maybe_note after
    `resource_.allocate` (pmr / large / small-pool-fallback).
  * AC3_TESTS — the three named suites cite ac3214_*.
  * AC4_NO_SECOND_REGISTRY — no new pin registry; no docs/design/3214-*
    (#1655); no tests/issues/test_issue_3214.cpp (#81967).
  * AC5_SOFT_ZERO_COST — maybe_note still loads required_active before
    any with_cover_ work.

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _maybe_note_body(arena: str) -> str:
    m = re.search(
        r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)[^{]*\{",
        arena,
    )
    if not m:
        # Multi-line signature: first `)` is the `= nullptr)` of slot.
        m = re.search(r"void\s+maybe_note_allocate_intermediate_\s*\(", arena)
        if not m:
            return ""
        brace = arena.find("{", m.start())
        if brace < 0:
            return ""
        start = brace
    else:
        start = m.end() - 1
    depth = 0
    for i in range(start, len(arena)):
        if arena[i] == "{":
            depth += 1
        elif arena[i] == "}":
            depth -= 1
            if depth == 0:
                return arena[start : i + 1]
    return ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    arena = _read("src/core/arena.ixx")
    lp = _read("src/core/lifetime_pin.hh")
    fail_closed = _read("tests/core/test_moving_densify_fail_closed.cpp")
    cover = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    gate = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    must("kDensifyTrackedAllocateCoverIssue = 3214", "AC1 arena stamp", arena)
    must("kDensifyTrackedAllocateCoverIssue = 3214", "AC1 lifetime_pin stamp", lp)
    must("Issue #3214", "AC1 arena cite", arena)

    body = _maybe_note_body(arena)
    if not body:
        fails.append("AC1: maybe_note_allocate_intermediate_ body not found")
    else:
        if "general_object_pin_required_active" not in body:
            fails.append("AC5: maybe_note missing required_active load")
        if "in_render_hotpath" not in body:
            fails.append("AC5: maybe_note missing in_render_hotpath gate")
        if "kMaxSmallSize" not in body:
            fails.append("AC1: maybe_note dropped kMaxSmallSize identity (#3180)")
        if "small_pool_.owns" not in body:
            fails.append("AC1: maybe_note dropped small_pool_.owns identity (#3180)")
        if "note_intermediate_create_with_cover_" not in body:
            fails.append("AC1: maybe_note does not route through with_cover_")
        if "non-small / pmr-fallback densify-tracked allocate" not in body:
            fails.append("AC1: maybe_note missing #3214 non-small branch")
        # Historical skip: size > kMaxSmallSize || !owns → return (no note).
        skip = re.search(
            r"if\s*\(\s*size\s*>\s*SmallObjectPool::kMaxSmallSize"
            r"\s*\|\|\s*!small_pool_\.owns\s*\(\s*ptr\s*\)\s*\)\s*"
            r"(?:return\s*;| \{\s*return\s*;\s*\})",
            body,
        )
        if skip:
            fails.append("AC1: maybe_note still early-returns on non-small / !owns")
        req = body.find("general_object_pin_required_active")
        cover_call = body.find("note_intermediate_create_with_cover_")
        if req >= 0 and cover_call >= 0 and cover_call < req:
            fails.append("AC5: with_cover_ before required_active load (Soft cost)")

    impl = re.search(
        r"void\s*\*\s*allocate_raw_impl\s*\([^)]*cover_slot\s*=\s*nullptr[^)]*\{",
        arena,
    )
    if not impl:
        # Multi-line signature.
        impl_pos = arena.find("void* allocate_raw_impl(")
        if impl_pos < 0:
            fails.append("AC2: allocate_raw_impl not found")
            impl_body = ""
        else:
            brace = arena.find("{", impl_pos)
            impl_body = arena[brace : brace + 2500] if brace >= 0 else ""
    else:
        impl_body = arena[impl.end() - 1 : impl.end() + 2500]
    pmr = impl_body.find("resource_.allocate(size, alignment)") if impl_body else -1
    if pmr < 0:
        fails.append("AC2: allocate_raw_impl pmr path not found")
    else:
        after = impl_body[pmr:]
        if "maybe_note_allocate_intermediate_(ptr, size, cover_slot, cover_reason)" not in after:
            fails.append("AC2: pmr / large allocate does not call maybe_note")

    must("ac3214_1_nonsmall_allocate_blocks", "AC3 fail_closed", fail_closed)
    must("ac3214_2_nonsmall_slot_cover_allows", "AC3 fail_closed slot", fail_closed)
    must("ac3214_3_soft_nonsmall_zero_cost", "AC3 fail_closed Soft", fail_closed)
    must("ac3214_nonsmall_allocate_notes_cover", "AC3 required-cover suite", cover)
    must("ac3214_coverage_gate_cite", "AC3 coverage-gate", gate)
    must("check_production_all_densify_allocate_cover_3214", "AC3 build.py", build)

    if "g_moving_pin_registry_3214" in arena or "class DensifyPinRegistry" in arena:
        fails.append("AC4: second pin registry introduced")
    if _read("docs/design/3214-densify-allocate-cover.md"):
        fails.append("AC4: docs/design/3214-* present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3214-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")
    if _read("tests/core/test_issue_3214.cpp") or _read("tests/issues/test_issue_3214.cpp"):
        fails.append("AC4: test_issue_3214.cpp present (forbidden #81967)")
    if "g_3214_" in arena:
        fails.append("AC4: invented g_3214_* counter (reuse uncovered metric)")

    if fails:
        print(f"Issue #3214 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3214 densify-tracked allocate cover — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
