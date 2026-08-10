#!/usr/bin/env python3
"""Issue #2874: pluggable swarm intelligence stdlib family.

Contract:
  AC1 lib/std/swarm.aura: swarm:init/step!/best/report/export + kinds grid|pso|ant
  AC2 lib/std/pso.aura thin surface; INDEX + adaptive help entries
  AC3 suite swarm_2874.aura + examples/swarm_sphere_search.aura
  AC4 parallel flat fiber path (no join-in-worker); wired in build.py
  AC5 no docs/design/2874-* / no test_issue_2874.cpp

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
(require "std/swarm" all:)
(define (sphere ind)
  (let loop ((xs ind) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))
(swarm:init (hash "kind" "pso" "dim" 1 "pop" 8
                  "bounds" (list (list -2.0 2.0)) "seed" 1))
(let loop ((i 0))
  (if (>= i 10) #t (begin (swarm:step! sphere) (loop (+ i 1)))))
(display (swarm:gen)) (newline)
(display (hash-ref (swarm:report) "kind")) (newline)
(display (number? (hash-ref (swarm:report) "best-fit"))) (newline)
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
    if "unbound variable" in out:
        fails.append(f"live smoke: unbound\n{out[:500]}")
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    if lines[:3] != ["10", "pso", "#t"]:
        fails.append(f"live smoke: expected ['10','pso','#t'], got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    swarm = _read("lib/std/swarm.aura")
    pso = _read("lib/std/pso.aura")
    index = _read("lib/std/INDEX.aura")
    adaptive = _read("lib/std/adaptive.aura")
    suite = _read("tests/suite/swarm_2874.aura")
    example = _read("examples/swarm_sphere_search.aura")
    build = _read("build.py")

    # AC1
    must("#2874", "AC1", swarm)
    must("swarm:init", "AC1", swarm)
    must("swarm:step!", "AC1", swarm)
    must("swarm:best", "AC1", swarm)
    must("swarm:report", "AC1", swarm)
    must("swarm:export", "AC1", swarm)
    must('"pso"', "AC1", swarm)
    must('"ant"', "AC1", swarm)
    must('"grid"', "AC1", swarm)
    must("pheromone:update", "AC1", swarm)
    must("fiber:spawn", "AC1", swarm)
    must("fiber:join", "AC1", swarm)

    # AC2
    must("pso:init", "AC2", pso)
    must('"swarm"', "AC2", index)
    must('"pso"', "AC2", index)
    must("std/swarm", "AC2", adaptive)
    must("std/pso", "AC2", adaptive)

    # AC3
    must("OK-2874", "AC3", suite)
    must("sphere", "AC3", suite)
    must("swarm:init", "AC3", example)
    must("OK swarm example", "AC3", example)

    # AC4
    must("check_swarm_2874", "AC4", build)
    must("no join-in-worker", "AC4", swarm)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2874.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2874.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2874-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2874 swarm stdlib — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
