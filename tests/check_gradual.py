#!/usr/bin/env python3
"""Gradual Guarantee verifier — annotated code produces same result as erased code."""

import os
import subprocess
import sys
from pathlib import Path

AURA = os.environ.get("AURA_BIN", str(Path(__file__).resolve().parent.parent / "build" / "aura"))

SCENARIOS = [
    ("int_annot", "(: x Int 42)", "42", "42"),
    ("expr_annot", "(: x Int (+ 1 2))", "(+ 1 2)", "3"),
    ("let_simple", "(let ((x 10)) x)", "(let ((x 10)) x)", "10"),
    (
        "poly_id",
        "(let ((id (lambda (x) x))) (id 42))",
        "(let ((id (lambda (x) x))) (id 42))",
        "42",
    ),
    (
        "poly_id_str",
        '(let ((id (lambda (x) x))) (id "hello"))',
        '(let ((id (lambda (x) x))) (id "hello"))',
        '"hello"',
    ),
    ("add", "(+ 1 2)", "(+ 1 2)", "3"),
    ("mul", "(* 6 7)", "(* 6 7)", "42"),
    ("lambda_call", "((lambda (x) (* x 2)) 5)", "((lambda (x) (* x 2)) 5)", "10"),
    ("if_true", "(if 1 42 0)", "(if 1 42 0)", "42"),
    ("if_false", "(if 0 42 99)", "(if 0 42 99)", "99"),
    (
        "fact_5",
        "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))",
        "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))",
        "120",
    ),
    ("str_append", '(string-append "a" "b")', '(string-append "a" "b")', '"ab"'),
    ("cons_car", "(car (cons 1 2))", "(car (cons 1 2))", "1"),
    (
        "nested_let",
        "(let ((x 10)) (let ((y 20)) (+ x y)))",
        "(let ((x 10)) (let ((y 20)) (+ x y)))",
        "30",
    ),
]


def run(code):
    cmd = [str(AURA)]
    try:
        r = subprocess.run(cmd, input=code.encode(), capture_output=True, timeout=5)
        return r.stdout.decode().strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "<TIMEOUT>", -1


def main():
    verbose = "--verbose" in sys.argv

    if not Path(AURA).exists():
        print(f"Error: {AURA} not found")
        return 1

    passed = failed = 0
    for name, annot, erased, expected in SCENARIOS:
        out_a, _ = run(annot)
        out_e, _ = run(erased)
        a_ok = out_a == expected
        e_ok = out_e == expected
        if a_ok and e_ok:
            if verbose:
                print(f"  PASS {name}: annot='{out_a}' erased='{out_e}'")
            passed += 1
        else:
            msg = []
            if not a_ok:
                msg.append(f"annot='{out_a}' (want '{expected}')")
            if not e_ok:
                msg.append(f"erased='{out_e}' (want '{expected}')")
            print(f"  FAIL {name}: {', '.join(msg)}")
            failed += 1

    total = passed + failed
    print(f"\nGradual guarantee: {passed}/{total} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
