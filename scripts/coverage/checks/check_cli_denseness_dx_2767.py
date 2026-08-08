#!/usr/bin/env python3
"""Issue #2767: denseness CLI DX — file path, -e, improved usage.

Denseness span runners hit stdin-only + bare usage footguns. Contract:

  AC1 main.cpp accepts -e / --eval and bare file.aura paths
  AC2 usage prints denseness env (AURA_PATH, AURA_SANDBOX, AURA_PIPELINE_STRICT)
  AC3 empty stdin / missing file gives explicit error + usage (not silent)
  AC4 piped stdin path preserved (isatty / source_from_argv)
  AC5 this linter wired in build.py; optional smoke if ./build/aura exists

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import os
import subprocess
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

    # AC5 — linter wire + optional smoke.
    must("check_cli_denseness_dx_2767", "AC5", build)
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("2767-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    aura = ROOT / "build" / "aura"
    if aura.is_file() and os.access(aura, os.X_OK):
        env = os.environ.copy()
        env.setdefault("AURA_SANDBOX", "off")
        env.setdefault("AURA_PIPELINE_STRICT", "0")
        # -e one-liner
        r = subprocess.run(
            [str(aura), "-e", "(+ 1 2)"],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        out = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "3" not in out:
            fails.append(f"AC5 smoke: -e '(+ 1 2)' failed exit={r.returncode} out={out[:200]!r}")
        # help mentions denseness env
        r2 = subprocess.run(
            [str(aura), "--help"],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        help_out = (r2.stdout or "") + (r2.stderr or "")
        if "AURA_PATH" not in help_out or "AURA_PIPELINE_STRICT" not in help_out:
            fails.append("AC5 smoke: --help missing denseness env knobs")
        # file path
        tmp = ROOT / "build" / "_aura_2767_smoke.aura"
        try:
            tmp.write_text("(display 42)(newline)\n", encoding="utf-8")
            r3 = subprocess.run(
                [str(aura), str(tmp)],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )
            out3 = (r3.stdout or "") + (r3.stderr or "")
            if r3.returncode != 0 or "42" not in out3:
                fails.append(f"AC5 smoke: file path failed exit={r3.returncode} out={out3[:200]!r}")
        finally:
            if tmp.is_file():
                tmp.unlink()
        # stdin still works
        r4 = subprocess.run(
            [str(aura)],
            input="(display 7)(newline)\n",
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        out4 = (r4.stdout or "") + (r4.stderr or "")
        if r4.returncode != 0 or "7" not in out4:
            fails.append(f"AC5 smoke: stdin pipe failed exit={r4.returncode} out={out4[:200]!r}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2767 denseness CLI DX (file/-e/usage) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
