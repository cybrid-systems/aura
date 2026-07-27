#!/usr/bin/env python3
"""Issue #2217: known TUI/render hot prims must use register_render_hot_prim.

AC4 static gate:
  - Helper API exists in render_prim_template.hh
  - kRenderHotPrimNamesRequired lists the known hot names
  - evaluator_primitives_tui.cpp registers each via register_render_hot_prim
  - No ad-hoc set_meta_for_name(RENDER_PRIMITIVE_META) for those names after
    migration (bare add("name") for required names is a failure)

Usage:
  python3 scripts/check_register_render_hot_prim_coverage.py
  python3 scripts/check_register_render_hot_prim_coverage.py --strict
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Mirrors kRenderHotPrimNamesRequired in render_prim_template.hh
REQUIRED = (
    "tui:present",
    "tui:cell",
    "tui:clear",
    "tui:present-dirty",
    "tui:draw-batch",
    "tui:fill-rect",
    "tui:present-batch",
    "tui:frame-ansi",
)

HELPER_RE = re.compile(r'register_render_hot_prim\s*\(\s*add\s*,\s*ev\s*,\s*"([^"]+)"')
# Bare add("name" that bypasses the helper for a known hot name.
BARE_ADD_RE = re.compile(r'(?:^|[^\w.])add\(\s*"([^"]+)"')


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list[str]) -> None:
    if not cond:
        fails.append(msg)


def check() -> list[str]:
    fails: list[str] = []
    tpl = _read("src/compiler/render_prim_template.hh")
    tui = _read("src/compiler/evaluator_primitives_tui.cpp")
    test = _read("tests/compiler/test_register_render_hot_prim_2217.cpp")

    # AC1 — helper API surface
    _must(
        "register_render_hot_prim" in tpl and "template" in tpl,
        "AC1: register_render_hot_prim template missing in render_prim_template.hh",
        fails,
    )
    _must(
        "kRenderHotPrimNamesRequired" in tpl,
        "AC1: kRenderHotPrimNamesRequired table missing",
        fails,
    )
    _must(
        "g_register_render_hot_prim_total" in tpl or "kRegisterRenderHotPrimIssue" in tpl,
        "AC1: registration counter / issue stamp missing",
        fails,
    )
    _must(
        "MUST use register_render_hot_prim" in tpl or "REQUIRED for new hot prims" in tpl,
        "AC3: template docs do not require helper for new hot prims",
        fails,
    )
    for name in REQUIRED:
        _must(
            f'"{name}"' in tpl,
            f"AC1: {name!r} missing from kRenderHotPrimNamesRequired",
            fails,
        )

    # AC2 — TUI migration
    registered = set(HELPER_RE.findall(tui))
    for name in REQUIRED:
        _must(
            name in registered,
            f"AC2: {name!r} not registered via register_render_hot_prim in TUI TU",
            fails,
        )
        # Bare add("name" must not remain for required hot names
        bare = set(BARE_ADD_RE.findall(tui))
        _must(
            name not in bare,
            f"AC2/AC4: {name!r} still uses bare add() instead of helper",
            fails,
        )

    # AC2 — no residual ad-hoc RENDER_PRIMITIVE_META set_meta in TUI TU
    _must(
        "set_meta_for_name" not in tui or tui.count("set_meta_for_name") == 0,
        "AC2: residual set_meta_for_name in evaluator_primitives_tui.cpp",
        fails,
    )
    _must(
        "RENDER_PRIMITIVE_META" not in tui,
        "AC2: residual RENDER_PRIMITIVE_META in TUI TU (use helper)",
        fails,
    )

    # AC5 — test surface
    _must(
        test and "#2217" in test,
        "AC5: tests/compiler/test_register_render_hot_prim_2217.cpp missing or no #2217 cite",
        fails,
    )
    _must(
        "register_render_hot_prim" in test,
        "AC5: test does not exercise/assert helper",
        fails,
    )
    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2217 register_render_hot_prim coverage linter")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero on any failure (CI / gate default)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Alias for running the check (exit 0 if clean)",
    )
    args = parser.parse_args()
    fails = check()
    if fails:
        print(f"check_register_render_hot_prim_coverage: {len(fails)} failure(s)", file=sys.stderr)
        for f in fails:
            print(f"  FAIL: {f}", file=sys.stderr)
        if args.strict or args.self_test:
            return 1
        return 0
    print("check_register_render_hot_prim_coverage: OK (#2217)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
