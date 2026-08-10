#!/usr/bin/env python3
"""Issue #2876: std/ant mutation-type ranking as swarm kind:ant.

Contract:
  AC1 ant.aura: evaporate!, types, rank; colony:search still exported
  AC2 swarm kind:ant uses pheromone:rank/update + ops/evaporate opts
  AC3 suite + example swarm_ant_rank.aura; docs/stdlib/ant.md + swarm.md
  AC4 linter wired in build.py; live smoke
  AC5 no docs/design/2876-* / no test_issue_2876.cpp

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
(require "std/ant" all:)
(define ops (list "edsl-lit-tweak" "edsl-op-swap" "edsl-disp-ref"))
(define (fit op) (if (string=? op "edsl-lit-tweak") 1.0 0.1))
(swarm:init (hash "kind" "ant" "pop" 3 "ops" ops "evaporate" 0.95))
(let loop ((i 0))
  (if (>= i 6) #t (begin (swarm:step! fit) (loop (+ i 1)))))
(display (swarm:best)) (newline)
(display (string=? (swarm:best) "edsl-lit-tweak")) (newline)
(display (procedure? colony:search)) (newline)
(display (procedure? pheromone:evaporate!)) (newline)
"""
    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        r = subprocess.run(
            [str(aura), "--load", "/dev/stdin"],
            input=code,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
            cwd=str(ROOT),
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f"live smoke: {e}"]
    # --load /dev/stdin may not work; try without --load
    if r.returncode != 0 or "unbound" in ((r.stdout or "") + (r.stderr or "")):
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
    # Prefer writing a temp file for reliability
    tmp = ROOT / "build" / "_smoke_2876.aura"
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
    if lines[:4] != ["edsl-lit-tweak", "#t", "#t", "#t"]:
        fails.append(f"live smoke: expected lit-tweak + flags, got {lines[:8]!r}\n{out[:500]}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ant = _read("lib/std/ant.aura")
    swarm = _read("lib/std/swarm.aura")
    suite = _read("tests/suite/swarm_ant_bridge_2876.aura")
    example = _read("examples/swarm_ant_rank.aura")
    docs_ant = _read("docs/stdlib/ant.md")
    docs_sw = _read("docs/stdlib/swarm.md")
    build = _read("build.py")

    # AC1
    must("pheromone:evaporate!", "AC1", ant)
    must("pheromone:types", "AC1", ant)
    must("colony:search", "AC1", ant)
    must("#2876", "AC1", ant)
    must("mutation-type", "AC1", ant)

    # AC2
    must("swarm-step-ant!", "AC2", swarm)
    must("pheromone:rank", "AC2", swarm)
    must("pheromone:update", "AC2", swarm)
    must("pheromone:evaporate!", "AC2", swarm)
    must("*swarm-ant-ops*", "AC2", swarm)
    must("ops", "AC2", swarm)
    must("evaporate", "AC2", swarm)
    must("#2876", "AC2", swarm)

    # AC3
    must("OK-2876", "AC3", suite)
    must("colony:search", "AC3", suite)
    must("edsl-lit-tweak", "AC3", example)
    must("OK swarm ant rank example", "AC3", example)
    must("#2876", "AC3", docs_ant)
    must("mutation-type", "AC3", docs_sw)

    # AC4
    must("check_swarm_ant_bridge_2876", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2876.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2876.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2876-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2876 swarm ant bridge — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
