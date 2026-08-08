#!/usr/bin/env python3
"""Issue #2770: std/string string-split O(1)-stack iterative rewrite.

string-split was a non-tail recursive walk over every character (plus
nested reverse of char lists) → recursion depth exceeded (>700) on
~400-char inputs and multi-line mailboxes (Hermes Phase 5).

Contract (one row per AC):
  AC1 string-split / string-split-words / string-repeat use while
      (no recursive iter over index on hot path)
  AC2 commercial_readiness + suite regressions for 5k / 2k-lines
  AC3 live stdin smoke (when build/aura exists): 5000 oneshot + 2000 lines
  AC4 this linter wired in build.py; no docs/design/2770-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _extract_define(src: str, name: str) -> str:
    """Best-effort extract of a top-level (define (name …) …) form body."""
    m = re.search(rf"(?m)^\((?:define\s+\({re.escape(name)}\b)", src)
    if not m:
        return ""
    start = m.start()
    depth = 0
    i = start
    in_str = False
    while i < len(src):
        c = src[i]
        if in_str:
            if c == "\\" and i + 1 < len(src):
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == ";":
            while i < len(src) and src[i] != "\n":
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
        i += 1
    return src[start:]


def live_smoke() -> list[str]:
    aura = ROOT / "build" / "aura"
    if not aura.is_file() or not os.access(aura, os.X_OK):
        return []
    code = r"""
(require "std/string" all:)
(define s (string-repeat "a" 5000))
(display (length (string-split s "|"))) (newline)
(define lines (string-repeat "a\n" 2000))
(display (length (string-split lines "\n"))) (newline)
(display (length (string-split-words (string-repeat "w " 500)))) (newline)
(display (equal? (string-split "a,b,c" ",") (list "a" "b" "c"))) (newline)
"""
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        r = subprocess.run(
            [str(aura)],
            input=code,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
            cwd=str(ROOT),
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f"live smoke: {e}"]
    out = (r.stdout or "") + (r.stderr or "")
    fails: list[str] = []
    if "recursion depth exceeded" in out:
        fails.append(f"live smoke: recursion depth exceeded\n{out[:500]}")
    if "unbound variable" in out:
        fails.append(f"live smoke: unbound variable\n{out[:500]}")
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    # Expected: 1, 2001, 500, #t
    expected = ["1", "2001", "500", "#t"]
    if lines[:4] != expected:
        fails.append(f"live smoke: expected {expected}, got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden residual {n!r}")

    src = _read("lib/std/string.aura")
    suite = _read("tests/suite/stdlib.aura")
    e2e = _read("tests/e2e/commercial_readiness/commercial_readiness_stdlib_string.aura")
    build = _read("build.py")

    # ── AC1: iterative rewrite present ──
    must("#2770", "AC1", src)
    must("while", "AC1", src)
    split = _extract_define(src, "string-split")
    words = _extract_define(src, "string-split-words")
    repeat = _extract_define(src, "string-repeat")
    if not split:
        fails.append("AC1: could not extract string-split define")
    else:
        must("while", "AC1 string-split", split)
        must("substring", "AC1 string-split", split)
        # Old recursive pattern: (define (iter i current result)
        must_not("(define (iter i current result)", "AC1 string-split", split)
        must_not("(iter (+ i 1)", "AC1 string-split", split)
    if not words:
        fails.append("AC1: could not extract string-split-words define")
    else:
        must("while", "AC1 string-split-words", words)
        must_not("(define (iter i current result)", "AC1 string-split-words", words)
    if not repeat:
        fails.append("AC1: could not extract string-repeat define")
    else:
        must("while", "AC1 string-repeat", repeat)
        must_not("(string-repeat s (- n 1))", "AC1 string-repeat", repeat)

    # ── AC2: suite + commercial readiness regressions ──
    must("#2770", "AC2", suite)
    must("5000", "AC2", suite)
    must("2000", "AC2", suite)
    must("string-split-words", "AC2", suite)
    must("#2770", "AC2", e2e)
    must("split-5k-oneshot", "AC2", e2e)
    must("split-2k-lines", "AC2", e2e)
    must("split-words-long", "AC2", e2e)

    # ── AC3: live smoke when aura binary present ──
    fails.extend(f"AC3: {m}" for m in live_smoke())

    # ── AC4: wire + no docs/design ──
    must("check_string_split_iterative_2770", "AC4", build)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2770-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2770.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_2770.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2770 string-split iterative O(1)-stack — while rewrite + suite/e2e + live smoke green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
