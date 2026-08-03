#!/usr/bin/env python3
"""Issue #2217 / #2626: render hot prim registrar coverage.

Historically (#2217) required tui:* hot prims to register via
register_render_hot_prim. Issue #2626 removed the entire tui:* / terminal
present surface — this gate now locks that removal:

  AC1: evaluator_primitives_tui.cpp is gone
  AC2: no tui: / terminal: public prims in source (add("tui:…") / add("terminal:…"))
  AC3: helper template may remain for residual render: tools (optional)
  AC4: no tests/renderer tree
  AC5: build.py still wires this check (regression fence)

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

ADD_TUI_RE = re.compile(r'\badd\(\s*"(tui:[^"]+|terminal:[^"]+)"')


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

    tui_cpp = REPO / "src/compiler/evaluator_primitives_tui.cpp"
    _must(not tui_cpp.is_file(), "AC1: evaluator_primitives_tui.cpp must be deleted (#2626)", fails)

    renderer = REPO / "src/renderer"
    _must(not renderer.exists(), "AC4: src/renderer/ must be gone (#2625/#2626)", fails)
    tests_renderer = REPO / "tests/renderer"
    _must(not tests_renderer.exists(), "AC4: tests/renderer/ must be gone (#2625/#2626)", fails)

    # Scan prim registration TUs for residual tui:/terminal: add()
    scan_roots = [
        REPO / "src/compiler",
    ]
    for root in scan_roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in {".cpp", ".ixx", ".h", ".hh"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for m in ADD_TUI_RE.finditer(text):
                fails.append(f"AC2: residual {m.group(1)!r} in {path.relative_to(REPO)}")

    build = _read("build.py")
    _must(
        "register-render-hot-prim" in build or "cmd_register_render_hot_prim_coverage" in build,
        "AC5: build.py gate command missing",
        fails,
    )
    _must(
        "#2626" in _read("scripts/check_register_render_hot_prim_coverage.py"),
        "AC5: this script must cite #2626",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2217/#2626 register_render_hot_prim / TUI removal coverage")
    parser.add_argument("--strict", action="store_true", help="alias for default strict exit")
    _ = parser.parse_args()
    fails = check()
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"check_register_render_hot_prim_coverage: {len(fails)} failure(s)",
            file=sys.stderr,
        )
        return 1
    print("check_register_render_hot_prim_coverage: OK (#2217 retired / #2626 TUI removed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
