#!/usr/bin/env python3
"""Issue #2872: define RHS failure must not leave wrong/void binding.

Previously top-level (define name expr) pre-bound a void cell, then
evaluated RHS. On recursion-depth / unexpected failure the name stayed
bound (void or stale alias) and multi-define could look like a top-level
wipe. Fix: non-lambda RHS eval-then-bind; Lambda pre-bind with unbind
rollback; multi-define rolls back still-void cells on phase-2 failure.

Contract:
  AC1 Env::unbind_local / unbind_local_symid present
  AC2 Define path: rhs_is_lambda / commit-on-success / rollback unbind
  AC3 multi-define rollback_still_void_defs
  AC4 suite define_rhs_fail_bind_2872.aura
  AC5 linter wired in build.py; live smoke when aura exists
  AC6 no docs/design/2872-* / no test_issue_2872.cpp

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
(define (deep-nontail n)
  (if (<= n 0) 0
    (let ((x (deep-nontail (- n 1)))) (+ 1 x))))
(define a 10)
(define b (deep-nontail 40))
(display b) (newline)
(define c (deep-nontail 900))
(display a) (newline)
(display b) (newline)
(display c) (newline)
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
    if "recursion depth exceeded" not in out:
        fails.append(f"live smoke: expected depth error\n{out[:500]}")
    if "unbound variable: c" not in out:
        fails.append(f"live smoke: expected unbound variable: c\n{out[:500]}")
    # Must not print void for c as a successful value before unbound
    lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    # Expected: b=40, then a=10, b=40; c should error not print ()
    if "40" not in lines:
        fails.append(f"live smoke: expected b=40 in stdout, got {lines!r}")
    if "10" not in lines:
        fails.append(f"live smoke: expected a=10 after failed c, got {lines!r}")
    # Sibling beta-class: a and b still readable
    if "unbound variable: a" in out or "unbound variable: b" in out:
        fails.append(f"live smoke: sibling unbound\n{out[:500]}")
    # Must not alias c to b (eq path) — if c printed as 40 that would be wrong
    if lines.count("40") >= 3 and "unbound variable: c" not in out:
        fails.append(f"live smoke: possible c alias of b: {lines!r}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    env_cpp = _read("src/compiler/evaluator_env.cpp")
    env_ixx = _read("src/compiler/evaluator.ixx")
    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    suite = _read("tests/suite/define_rhs_fail_bind_2872.aura")
    build = _read("build.py")

    # AC1
    must("unbind_local", "AC1", env_ixx)
    must("unbind_local_symid", "AC1", env_ixx)
    must("bool Env::unbind_local", "AC1", env_cpp)
    must("bool Env::unbind_local_symid", "AC1", env_cpp)
    must("#2872", "AC1", env_cpp)

    # AC2
    must("rhs_is_lambda", "AC2", ev)
    must("Issue #2872", "AC2", ev)
    must("Eval first; bind only on success", "AC2", ev)
    must("Roll back so name is unbound", "AC2", ev)

    # AC3
    must("rollback_still_void_defs", "AC3", ev)

    # AC4
    must("2872", "AC4", suite)
    must("OK-2872", "AC4", suite)
    must("deep-nontail", "AC4", suite)

    # AC5 wire
    must("check_define_rhs_fail_bind_2872", "AC5", build)

    # AC6 forbidden artifacts
    if (ROOT / "tests" / "compiler" / "test_issue_2872.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2872.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2872-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    fails.extend(live_smoke())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2872 define RHS fail bind — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
