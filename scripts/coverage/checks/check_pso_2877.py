#!/usr/bin/env python3
"""Issue #2877: std/pso particle swarm backend for parameter search.

Contract:
  AC1 swarm.aura: configurable w/c1/c2; pso default pop 16
  AC2 pso.aura: init/init-hash/step/best/population/defaults/help
  AC3 docs/stdlib/pso.md when-to-use + suite pso_2877.aura
  AC4 linter wired in build.py; live smoke sphere
  AC5 no docs/design/2877-* / no test_issue_2877.cpp

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
(require "std/pso" all:)
(define (sphere-fit x)
  (let loop ((xs x) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))
(pso:init 1 16 (list (list -2.0 2.0)) 7)
(let loop ((i 0))
  (if (>= i 15) #t (begin (pso:step! sphere-fit) (loop (+ i 1)))))
(display (length (pso:population))) (newline)
(display (string=? (hash-ref (pso:report) "kind") "pso")) (newline)
(display (number? (hash-ref (pso:report) "best-fit"))) (newline)
"""
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    tmp = ROOT / "build" / "_smoke_2877.aura"
    try:
        tmp.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(code, encoding="utf-8")
        r = subprocess.run(
            [str(aura), "--load", str(tmp)],
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
    if "unbound variable" in out:
        fails.append(f"live smoke: unbound\n{out[:500]}")
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    if lines[:3] != ["16", "#t", "#t"]:
        fails.append(f"live smoke: expected ['16','#t','#t'], got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    swarm = _read("lib/std/swarm.aura")
    pso = _read("lib/std/pso.aura")
    docs = _read("docs/stdlib/pso.md")
    suite = _read("tests/suite/pso_2877.aura")
    build = _read("build.py")
    _read("docs/stdlib/swarm.md")

    # AC1
    must("*swarm-pso-w*", "AC1", swarm)
    must("*swarm-pso-c1*", "AC1", swarm)
    must("*swarm-pso-c2*", "AC1", swarm)
    must('"w"', "AC1", swarm)
    must("#2877", "AC1", swarm)
    must("pop-default", "AC1", swarm)

    # AC2
    must("pso:init-hash", "AC2", pso)
    must("pso:defaults", "AC2", pso)
    must("pso:population", "AC2", pso)
    must("#2877", "AC2", pso)
    must("0.7", "AC2", pso)
    must("1.4", "AC2", pso)

    # AC3
    if not docs:
        fails.append("AC3: docs/stdlib/pso.md missing")
    else:
        must("When to use", "AC3", docs)
        must("PSO", "AC3", docs)
        must("grid", "AC3", docs)
        must("ant", "AC3", docs)
    must("OK-2877", "AC3", suite)
    must("sphere", "AC3", suite)
    must("bounds", "AC3", suite)
    must("deterministic", "AC3", suite)

    # AC4
    must("check_pso_2877", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2877.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2877.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2877-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2877 pso — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
