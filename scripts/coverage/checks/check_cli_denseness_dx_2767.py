#!/usr/bin/env python3
"""Issue #2767: denseness CLI DX — file path, -e, improved usage.

Denseness span runners hit stdin-only + bare usage footguns. Contract:

  AC1 main.cpp accepts -e / --eval and bare file.aura paths
  AC2 usage prints denseness env (AURA_PATH, AURA_SANDBOX, AURA_PIPELINE_STRICT)
  AC3 empty stdin / missing file gives explicit error + usage (not silent)
  AC4 piped stdin path preserved (isatty / source_from_argv)
  AC5 this linter wired in build.py (static only; no live aura smoke)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    main_cpp = _read("src/main.cpp")
    build = _read("build.py")

    # AC1 — file / -e sugar.
    must("#2767", "AC1", main_cpp)
    must("-e", "AC1", main_cpp)
    must("--eval", "AC1", main_cpp)
    must("source_from_argv", "AC1", main_cpp)
    must("cannot open program file", "AC1", main_cpp)

    # AC2 — denseness env in usage.
    must("print_denseness_usage", "AC2", main_cpp)
    must("AURA_PATH", "AC2", main_cpp)
    must("AURA_SANDBOX", "AC2", main_cpp)
    must("AURA_PIPELINE_STRICT", "AC2", main_cpp)
    must("#2213", "AC2", main_cpp)

    # AC3 — explicit empty / missing errors.
    must("no program source", "AC3", main_cpp)
    must("stdin empty", "AC3", main_cpp)
    must("--help", "AC3", main_cpp)

    # AC4 — stdin pipe preserved.
    must("isatty", "AC4", main_cpp)
    must("!source_from_argv", "AC4", main_cpp)
    must("cin.rdbuf", "AC4", main_cpp)

    # AC5 — linter wire (static only; live aura smoke removed from gate).
    must("check_cli_denseness_dx_2767", "AC5", build)
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("2767-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2767 denseness CLI DX (file/-e/usage) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
