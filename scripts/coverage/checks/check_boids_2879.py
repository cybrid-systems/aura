#!/usr/bin/env python3
"""Issue #2879: std/boids flocking coordination + swarm kind:boids.

Contract:
  AC1 boids.aura: separation/alignment/cohesion/step/diversity pure API
  AC2 swarm kind:boids uses boids:step; docs/stdlib/boids.md
  AC3 suite boids_2879.aura; INDEX entry
  AC4 linter wired in build.py; live smoke diversity under sep
  AC5 no docs/design/2879-* / no test_issue_2879.cpp

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
(require "std/boids" all:)
(define pos (list (list 0.0 0.0) (list 0.05 0.0) (list 0.1 0.0) (list 0.8 0.8)))
(define vel (list (list 0.0 0.0) (list 0.0 0.0) (list 0.0 0.0) (list 0.0 0.0)))
(define d0 (boids:diversity pos))
(define r (boids:step pos vel
  (hash "sep" 2.5 "align" 0.0 "cohere" 0.0 "radius" 0.6
        "max-speed" 0.4
        "bounds" (list (list -2.0 2.0) (list -2.0 2.0)))))
(define d1 (boids:diversity (car r)))
(display (> d1 d0)) (newline)
(display (list? (boids:separation pos 0 0.5))) (newline)
(require "std/swarm" all:)
(swarm:init (hash "kind" "boids" "dim" 2 "pop" 6 "seed" 1
                  "bounds" (list (list -1.0 1.0) (list -1.0 1.0))))
(display (string=? (swarm:kind) "boids")) (newline)
"""
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    tmp = ROOT / "build" / "_smoke_2879.aura"
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
    if lines[:3] != ["#t", "#t", "#t"]:
        fails.append(f"live smoke: expected three #t, got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    boids = _read("lib/std/boids.aura")
    swarm = _read("lib/std/swarm.aura")
    docs = _read("docs/stdlib/boids.md")
    suite = _read("tests/suite/boids_2879.aura")
    index = _read("lib/std/INDEX.aura")
    build = _read("build.py")

    # AC1
    must("boids:separation", "AC1", boids)
    must("boids:alignment", "AC1", boids)
    must("boids:cohesion", "AC1", boids)
    must("boids:step", "AC1", boids)
    must("boids:diversity", "AC1", boids)
    must("#2879", "AC1", boids)

    # AC2
    must("swarm-step-boids!", "AC2", swarm)
    must('"boids"', "AC2", swarm)
    must("#2879", "AC2", swarm)
    if not docs:
        fails.append("AC2: docs/stdlib/boids.md missing")
    else:
        must("Separation", "AC2", docs)
        must("sole global optimizer", "AC2", docs)
        must("multi-agent", "AC2", docs)

    # AC3
    must("OK-2879", "AC3", suite)
    must('"boids"', "AC3", index)

    # AC4
    must("check_boids_2879", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2879.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2879.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2879-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2879 boids — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
