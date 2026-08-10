#!/usr/bin/env python3
"""Issue #2873: multi-frame named-let past early depth caps + prim shadow.

#2871 made TW named-let true TCO (continue, not return eval_flat). #2873
locks the quantitative multi-frame case (string-append+cons / cache-put)
past former ~200 / ~120 thresholds, and fixes Call prim fast-path so
env-bound names (let take / map / drop) shadow list primitives.

Contract:
  AC1 Call Variable: env binding shadows prim (#2873 comment + is_primitive)
  AC2 #2871 TCO continue for TW closure still present
  AC3 suite multiframe_named_let_2873.aura
  AC4 linter wired in build.py; live smoke when aura exists
  AC5 no docs/design/2873-* / no test_issue_2873.cpp

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


def live_smoke() -> list[str]:
    aura = ROOT / "build" / "aura"
    if not aura.is_file() or not os.access(aura, os.X_OK):
        return []
    code = r"""
(define (deep-map n)
  (let loop ((i 0) (s (quote ())))
    (if (>= i n) (length s)
      (loop (+ i 1)
            (cons (string-append "k" (number->string i)) s)))))
(display (deep-map 2000)) (newline)
(display (let take ((ys (list 1 2 3)) (n 0) (out (quote ())))
  (if (or (null? ys) (>= n 10)) out
    (take (cdr ys) (+ n 1) (cons (car ys) out))))) (newline)
(display (take 2 (list 1 2 3 4))) (newline)
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
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    # deep-map 2000, take-rev (3 2 1), prim take (1 2)
    if not lines or lines[0] != "2000":
        fails.append(f"live smoke: expected deep-map 2000, got {lines[:6]!r}\n{out[:500]}")
    # accept printed list forms for take-rev / prim take
    if len(lines) < 2 or "3" not in lines[1]:
        fails.append(f"live smoke: expected take-rev list, got {lines[:6]!r}\n{out[:500]}")
    if len(lines) < 3 or "1" not in lines[2]:
        fails.append(f"live smoke: expected prim take, got {lines[:6]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    suite = _read("tests/suite/multiframe_named_let_2873.aura")
    build = _read("build.py")

    # AC1 — prim shadow
    must("#2873", "AC1", ev)
    must("only take the prim fast-path when the name is", "AC1", ev)
    must("is_primitive(*bound)", "AC1", ev)
    must("let take", "AC1", ev)

    # AC2 — TCO still present
    must("true TCO for TW closure calls", "AC2", ev)
    must("current_id = cl.body_id", "AC2", ev)

    # AC3 — suite
    must("2873", "AC3", suite)
    must("deep-map", "AC3", suite)
    must("cache-put", "AC3", suite)
    must("OK-2873", "AC3", suite)
    must("let take", "AC3", suite)

    # AC4 — wire
    must("check_multiframe_named_let_2873", "AC4", build)

    # AC5 — forbidden
    if (ROOT / "tests" / "compiler" / "test_issue_2873.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2873.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2873-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2873 multiframe named-let — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
