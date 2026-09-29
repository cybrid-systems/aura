#!/usr/bin/env python3
"""Hard-timeout Soft oneshot stdlib smokes for #4178–#4219.

Verifies Soft oneshot (`aura -e`) binds Soft std list/string/hash helpers
without aura-build soft_*.aura host fills. Prefer hard timeouts; no
task-specific gold overrides.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4132" / "aura")))
TIMEOUT_S = float(os.environ.get("AURA_ONESHOT_TIMEOUT", "8"))

# (issue, expr, expect_substr_in_stdout)
CASES: list[tuple[int, str, str]] = [
    (4178, '(string-take "ab" 1)', '"a"'),
    (4179, '(string-drop "ab" 1)', '"b"'),
    (4180, '(string-pad "a" 3)', '"  a"'),
    (4181, '(string-trim " a ")', '"a"'),
    (4182, '(string-split "a,b" ",")', '("a" "b")'),
    (4183, '(string-replace "ab" "a" "Z")', '"Zb"'),
    (4184, '(string-downcase "A")', '"a"'),
    (4185, '(string-upcase "a")', '"A"'),
    (4186, '(starts-with? "ab" "a")', "#t"),
    (4187, '(ends-with? "ab" "b")', "#t"),
    (4188, '(contains? "ab" "a")', "#t"),
    (4189, "(make-list 2 9)", "(9 9)"),
    (4190, "(for-each (lambda (x) x) (list 1))", "#t"),
    (4191, "(foldr + 0 (list 1 2))", "3"),
    (4192, "(any even? (list 1 2))", "#t"),
    (4193, "(all even? (list 2 4))", "#t"),
    (4194, "(list-tail (list 1 2 3) 1)", "(2 3)"),
    (4195, "(hash-for-each (make-hash) (lambda (k v) #t))", "#t"),
    (4196, "(hash-fold (make-hash) (lambda (k v a) a) 0)", "0"),
    (4197, "(hash-empty? (make-hash))", "#t"),
    (4198, "(null? (hash->list (make-hash)))", "#t"),
    (4199, "(all (lambda (x) (> x 0)) (quote (1 2 3)))", "#t"),
    (4200, "(any (lambda (x) (> x 2)) (quote (1 2 3)))", "#t"),
    (4201, "(foldr cons (quote ()) (quote (1 2 3)))", "(1 2 3)"),
    (
        4202,
        "(begin (define n 0) (for-each (lambda (x) (set! n (+ n x))) (quote (1 2 3))) n)",
        "6",
    ),
    (4203, "(make-list 3 7)", "(7 7 7)"),
    (
        4204,
        '(begin (define h (make-hash)) (hash-set! h "a" 1) (define n 0) '
        "(hash-for-each h (lambda (k v) (set! n (+ n v)))) n)",
        "1",
    ),
    (
        4205,
        '(begin (define h (make-hash)) (hash-set! h "a" 1) (hash-set! h "b" 2) '
        "(hash-fold h (lambda (k v acc) (+ acc v)) 0))",
        "3",
    ),
    (4206, "(hash-empty? (make-hash))", "#t"),
    (4207, "(null? (hash->list (make-hash)))", "#t"),
    (4208, '(string-pad "x" 3)', '"  x"'),
    (4209, '(string-take "abcd" 2)', '"ab"'),
    (4210, '(string-drop "abcd" 2)', '"cd"'),
    (4211, '(string-downcase "AbC")', '"abc"'),
    (4212, '(string-upcase "AbC")', '"ABC"'),
    (4213, '(string-trim "  hi  ")', '"hi"'),
    (4214, '(string-split "a,b,c" ",")', '("a" "b" "c")'),
    (4215, '(string-replace "ababa" "ba" "X")', '"aXX"'),
    (4216, '(contains? "foobar" "oba")', "#t"),
    (4217, '(string-starts-with "foobar" "foo")', "#t"),
    (4218, '(string-ends-with "foobar" "bar")', "#t"),
    (4219, "(last (quote (1 2 3)))", "3"),
]


def run_case(issue: int, expr: str, expect: str) -> tuple[bool, str]:
    if not AURA.is_file():
        return False, f"missing aura binary: {AURA}"

    def _spawn() -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(AURA), "-e", expr],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
            cwd=str(ROOT),
            env={
                **os.environ,
                "AURA_SANDBOX": os.environ.get("AURA_SANDBOX", "off"),
                # Issue #4150: stdlib oneshots verify values, not the pipeline
                # face — default the soft oneshot spawn to the diagnostics
                # face (build.py's own default) so gate-composed runs stay
                # deterministic; an explicit caller value still wins.
                "AURA_PIPELINE_STRICT": os.environ.get("AURA_PIPELINE_STRICT", "0"),
            },
        )

    try:
        proc = _spawn()
    except subprocess.TimeoutExpired:
        return False, f"TIMEOUT>{TIMEOUT_S}s"
    if proc.returncode is not None and proc.returncode < 0:
        # Externally SIGKILLed child (negative rc, empty streams): the shared
        # gate host's memory-pressure waves kill freshly forked interpreter
        # children — the chronic stdlib-oneshot exit=-9 flakes (#4152/#4154/
        # #4155; reproduced even at lane jobs=2, so not burst-only). A signal
        # death is not a case outcome: retry once so the assertions below run
        # on a live child. A real interpreter crash reproduces and still
        # fails; no value/exit/unbound assertion is relaxed.
        time.sleep(2.0)
        try:
            proc = _spawn()
        except subprocess.TimeoutExpired:
            return False, f"TIMEOUT>{TIMEOUT_S}s (after sigkill retry)"
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
