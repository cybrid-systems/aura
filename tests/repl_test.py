#!/usr/bin/env python3
"""REPL black-box integration tests using pexpect."""
import subprocess
import pexpect
import sys
import os

AURA_BIN = "./build/aura"
TIMEOUT = 5


def spawn_repl():
    """Spawn REPL with TERM=dumb to avoid ANSI escape sequences."""
    env = os.environ.copy()
    env["TERM"] = "dumb"
    child = pexpect.spawn(AURA_BIN, timeout=TIMEOUT, env=env, encoding='utf-8', codec_errors='replace')
    child.delaybeforesend = 0.1
    return child


def test_simple_eval():
    """Basic expression evaluation."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("(+ 1 2 3)")
    child.expect(r"6")
    child.sendline("(quit)")
    child.expect(pexpect.EOF)
    print("  ✓ simple_eval")


def test_define_and_call():
    """Define a function, then call it."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("(define (square x) (* x x))")
    child.expect(r"> ")
    child.sendline("(square 5)")
    child.expect(r"25")
    child.sendline("(quit)")
    print("  ✓ define_and_call")


def test_multiline():
    """Single-line equivalent of multiline expression."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("(let ((x 40)) (+ x 2))")
    child.expect(r"42")
    child.sendline("(quit)")
    print("  ✓ multiline")


def test_quit():
    """(quit) exits cleanly."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("(quit)")
    child.expect(pexpect.EOF)
    print("  ✓ quit")


def test_error_handling():
    """Error message is printed, REPL stays alive."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("(undefined-variable)")
    child.expect(r"error: unbound variable")
    child.expect(r"> ")  # REPL still alive
    child.sendline("(+ 1 2)")
    child.expect(r"3")
    child.sendline("(quit)")
    print("  ✓ error_handling")


def test_empty_input():
    """Empty input doesn't crash."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("")
    child.expect(r"> ")  # Still alive
    child.sendline("42")
    child.expect(r"42")
    child.sendline("(quit)")
    print("  ✓ empty_input")


def test_lambda():
    """Lambda as value."""
    child = spawn_repl()
    child.expect(r"> ")
    child.sendline("((lambda (x) (+ x 1)) 41)")
    child.expect(r"42")
    child.sendline("(quit)")
    print("  ✓ lambda")


if __name__ == "__main__":
    tests = [
        test_simple_eval,
        test_define_and_call,
        test_multiline,
        test_quit,
        test_error_handling,
        test_empty_input,
        test_lambda,
    ]

    failed = 0
    for test in tests:
        try:
            test()
        except Exception as e:
            print(f"  ✗ {test.__name__}: {e}")
            failed += 1

    if failed:
        print(f"\n❌ {failed}/{len(tests)} tests failed")
        sys.exit(1)
    else:
        print(f"\n✅ All {len(tests)} REPL tests passed")
