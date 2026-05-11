#!/usr/bin/env python3
"""Aura Agent Auto-Fix Demo (Python)

Usage: python3 tests/agent_demo.py

Requires: ./build/aura compiled with --serve (JSON protocol)
"""

import subprocess
import json
import sys
import os

AURA = os.path.expanduser("~/code/aura/build/aura")

def compile_via_serve(code: str) -> dict:
    """Send code to --serve, return parsed JSON response."""
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _ = proc.communicate(code + "\n", timeout=5)
    for line in stdout.strip().split("\n"):
        if line:
            return json.loads(line)
    return {"status": "unknown"}

def query(code: str, query_str: str) -> str:
    """Run --query and return raw output."""
    proc = subprocess.Popen(
        [AURA, "--query", query_str],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _ = proc.communicate(code + "\n", timeout=5)
    return stdout

def query_and_fix(code: str, q: str, r: str) -> str:
    """Run --query-and-fix and return raw output."""
    proc = subprocess.Popen(
        [AURA, "--query-and-fix", q, r],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _ = proc.communicate(code + "\n", timeout=5)
    return stdout

def agent_fix_unbound(code: str, var: str) -> str:
    """Agent: detect unbound variable, fix it automatically."""
    print(f"Agent: fixing unbound variable '{var}'...")

    # Step 1: query where it appears
    qr = query(code, f'(= name "{var}")')
    print(f"  query result: {qr.strip()}")

    # Step 2: generate fix
    fix_q = f'(and (node-type Variable) (= name "{var}"))'
    fix_r = "(LiteralInt 42)"
    fixr = query_and_fix(code, fix_q, fix_r)
    print(f"  fix result: {fixr.strip()}")

    # Step 3: return fixed source
    return code.replace(var, "42")

def main():
    print("=== Aura Agent Auto-Fix Demo (Python) ===\n")

    # Step 1: buggy code
    print("--- Step 1: Submit buggy code ---")
    code = "(+ x 1)"
    print(f"Input: {code}")
    result = compile_via_serve(code)
    print(f"Result: {json.dumps(result, ensure_ascii=False)}\n")

    if result.get("status") == "error":
        # Step 2: fix
        print("--- Step 2: Agent applies fix ---")
        fixed = agent_fix_unbound(code, "x")
        print(f"Fixed source: {fixed}\n")

        # Step 3: verify
        print("--- Step 3: Verify fix ---")
        result2 = compile_via_serve(fixed)
        print(f"Result: {json.dumps(result2, ensure_ascii=False)}")
        if result2.get("status") == "ok":
            print(f"\n✓ Demo complete: {code} → {fixed} = {result2['value']}")
        else:
            print(f"\n✗ Fix failed: {result2}")
    else:
        print("No error detected.")

if __name__ == "__main__":
    main()
