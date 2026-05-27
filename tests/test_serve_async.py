#!/usr/bin/env python3
"""tests/test_serve_async.py — Test suite for --serve-async mode.

Runs test_serve_async.aura through --serve-async and validates output.

Usage:
  LLM_API_KEY="***" python3 tests/test_serve_async.py
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))

TEST_FILE = ROOT / "tests" / "test_serve_async.aura"

# Expected patterns in output
EXPECTED = [
    ("t1", "t1:basic"),
    ("t2", "t2:created:ok"),
    ("t3", "t3:worker"),
    ("t4-default", "t4:default"),
    ("t4-w1", "t4:w1"),
    ("t4-w2", "t4:w2"),
    ("t5", "t5:llm:"),
    ("t6-done", "t6:done"),
    ("t6-w3", "t6:w3:"),
    ("t6-w4", "t6:w4:"),
    ("t7", "t7:has-x:"),
    ("t8", "t8:all-tests-complete"),
]

# Tests that require LLM_API_KEY
LLM_TESTS = {"t5", "t6-w3", "t6-w4"}


def print_result(name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    print(f"  [{status}] {name} {detail}")


def main():
    if not TEST_FILE.exists():
        print(f"Error: {TEST_FILE} not found")
        sys.exit(1)

    print(f"=== Serve-Async Test Suite ===")
    print(f"  Aura: {AURA}")
    print(f"  Tests: {len(EXPECTED)} scenarios")

    has_key = bool(os.environ.get("LLM_API_KEY"))
    if not has_key:
        print(f"  Warning: LLM_API_KEY not set (skipping LLM tests)")

    # Run serve-async with test input
    start = time.time()
    proc = subprocess.Popen(
        [AURA, "--serve-async"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        close_fds=True,
    )

    # Send all test commands
    with open(TEST_FILE) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            proc.stdin.write(line + "\n")
    proc.stdin.flush()
    proc.stdin.close()

    # Read all output
    stdout, stderr = proc.communicate(timeout=60)
    elapsed = time.time() - start

    # Parse output lines
    output_lines = stdout.strip().split("\n")

    print(f"\n  Time: {elapsed:.1f}s")
    print(f"  Output lines: {len(output_lines)}")

    # Validate expected patterns
    all_passed = True
    for name, pattern in EXPECTED:
        # Skip LLM tests if no key
        if name in LLM_TESTS and not has_key:
            print_result(name, True, "(skipped, no API key)")
            continue

        found = any(pattern in line for line in output_lines)
        passed = found
        all_passed = all_passed and passed
        detail = f"(found: {pattern})" if found else f"(missing: {pattern})"
        print_result(name, passed, detail)

    # Check for protocol errors
    errors = [l for l in output_lines if '"status":"error"' in l]
    if errors:
        print(f"\n  Protocol errors ({len(errors)}):")
        for e in errors[:5]:
            print(f"    {e.strip()[:100]}")

    print(f"\n  {'ALL PASS' if all_passed else 'SOME FAILED'}")
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
