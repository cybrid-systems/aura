#!/usr/bin/env python3
"""Coercion elimination benchmark — measure IR instruction reduction."""
import subprocess, sys, os, time
from pathlib import Path

AURA = os.environ.get("AURA_BIN", str(Path(__file__).resolve().parent.parent / "build" / "aura"))

# Test programs that may generate CastOps
PROGRAMS = [
    ("annot-int",    '(: x Int 42)'),
    ("annot-expr",   '(: x Int (+ 1 2 3 4))'),
    ("nested-annot", '(+ (: a Int 10) (: b Int 20))'),
    ("lambda-call",  '((lambda (x) (* x 2)) 5)'),
    ("let-call",     '(let ((f (lambda (x) (+ x 1)))) (f 41))'),
    ("fact-5",       '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))'),
    ("poly-id",      '(let ((id (lambda (x) x))) (id 42) (id "hello"))'),
]

def time_run(code):
    start = time.perf_counter()
    r = subprocess.run([AURA], input=code.encode(), capture_output=True, timeout=5)
    elapsed = time.perf_counter() - start
    return r.stdout.decode().strip(), r.returncode, elapsed

def main():
    if not Path(AURA).exists():
        print(f"Error: {AURA} not found"); return 1

    for name, code in PROGRAMS:
        out, rc, t = time_run(code)
        status = "OK" if rc == 0 else f"FAIL(rc={rc})"
        print(f"  {status:6s} {name:15s} {t*1000:6.1f}ms  output={out[:30]}")

    print("\nAll coercion benchmarks complete")

if __name__ == "__main__":
    sys.exit(main())
