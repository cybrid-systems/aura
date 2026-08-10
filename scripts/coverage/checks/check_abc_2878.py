#!/usr/bin/env python3
"""Issue #2878: std/abc artificial bee colony backend.

Contract:
  AC1 swarm.aura: swarm-step-abc! employ/onlooker/scout + limit/neighbor
  AC2 abc.aura surface + docs/stdlib/abc.md vs PSO
  AC3 suite abc_2878.aura; INDEX entry
  AC4 linter wired in build.py; live smoke
  AC5 no docs/design/2878-* / no test_issue_2878.cpp

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
(require "std/abc" all:)
(define (sphere-fit x)
  (let loop ((xs x) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))
(abc:init 1 10 (list (list -2.0 2.0)) 5 7)
(let loop ((i 0))
  (if (>= i 12) #t (begin (abc:step! sphere-fit) (loop (+ i 1)))))
(display (string=? (hash-ref (abc:report) "kind") "abc")) (newline)
(display (number? (hash-ref (abc:report) "best-fit"))) (newline)
(display (length (abc:population))) (newline)
"""
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    tmp = ROOT / "build" / "_smoke_2878.aura"
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
    if lines[:3] != ["#t", "#t", "10"]:
        fails.append(f"live smoke: expected ['#t','#t','10'], got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    swarm = _read("lib/std/swarm.aura")
    abc = _read("lib/std/abc.aura")
    docs = _read("docs/stdlib/abc.md")
    suite = _read("tests/suite/abc_2878.aura")
    index = _read("lib/std/INDEX.aura")
    build = _read("build.py")

    # AC1
    must("swarm-step-abc!", "AC1", swarm)
    must("Employed", "AC1", swarm)
    must("Onlooker", "AC1", swarm)
    must("Scout", "AC1", swarm)
    must("*swarm-abc-limit*", "AC1", swarm)
    must("#2878", "AC1", swarm)
    must('"abc"', "AC1", swarm)

    # AC2
    must("abc:init", "AC2", abc)
    must("abc:init-hash", "AC2", abc)
    must("#2878", "AC2", abc)
    if not docs:
        fails.append("AC2: docs/stdlib/abc.md missing")
    else:
        must("PSO", "AC2", docs)
        must("Employed", "AC2", docs)
        must("Scout", "AC2", docs)

    # AC3
    must("OK-2878", "AC3", suite)
    must('"abc"', "AC3", index)

    # AC4
    must("check_abc_2878", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2878.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2878.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2878-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2878 abc — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
