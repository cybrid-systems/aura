#!/usr/bin/env python3
"""Hard-timeout Soft oneshot stdlib smokes for #4220–#4222.

Verifies Soft oneshot (`aura -e`) binds Soft std/list find + count and
exposes R7RS-order take (lst k) after Soft std no longer shadows native
with count-first take. Prefer Soft std primitives, not aura-build fills.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4132" / "aura")))
TIMEOUT_S = float(os.environ.get("AURA_ONESHOT_TIMEOUT", "8"))

# (issue, expr, expect_substr_in_stdout)
CASES: list[tuple[int, str, str]] = [
    # #4220 find (SRFI-1; Soft empty convention ())
    (4220, "(find even? (quote (1 2 3)))", "2"),
    (4220, "(null? (find even? (quote ())))", "#t"),
    (4220, "(null? (find (lambda (x) (> x 9)) (quote (1 2 3))))", "#t"),
    # #4221 Soft std take aligned to R7RS (lst k); drop stays native
    (4221, "(take (list 1 2 3 4) 2)", "(1 2)"),
    (4221, "(drop (list 1 2 3 4) 1)", "(2 3 4)"),
    (4221, "(skip 1 (list 1 2 3 4))", "(2 3 4)"),
    (4221, "(null? (take (list 1 2 3) 0))", "#t"),
    # #4222 count (SRFI-1)
    (4222, "(count (lambda (x) (= (modulo x 2) 0)) (quote (1 2 3 4)))", "2"),
    (4222, "(count (lambda (x) #t) (quote ()))", "0"),
    (4222, "(count even? (quote (1 3 5)))", "0"),
]


def run_case(issue: int, expr: str, expect: str) -> tuple[bool, str]:
    if not AURA.is_file():
        return False, f"missing aura binary: {AURA}"
    try:
        proc = subprocess.run(
            [str(AURA), "-e", expr],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
            cwd=str(ROOT),
            env={**os.environ, "AURA_SANDBOX": os.environ.get("AURA_SANDBOX", "off")},
        )
    except subprocess.TimeoutExpired:
        return False, f"TIMEOUT>{TIMEOUT_S}s"
    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    if proc.returncode != 0:
        return False, f"exit={proc.returncode} stderr={err!r} stdout={out!r}"
    if "unbound variable" in err.lower() or "unbound variable" in out.lower():
        return False, f"unbound: stderr={err!r} stdout={out!r}"
    if expect not in out:
        return False, f"expected {expect!r} in stdout={out!r} stderr={err!r}"
    return True, out


def main() -> int:
    print(f"AURA_BIN={AURA}")
    failed = 0
    for issue, expr, expect in CASES:
        ok, detail = run_case(issue, expr, expect)
        mark = "PASS" if ok else "FAIL"
        print(f"#{issue} {mark}: {expr} -> {detail}")
        if not ok:
            failed += 1
    print(f"summary: {len(CASES) - failed}/{len(CASES)} pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
