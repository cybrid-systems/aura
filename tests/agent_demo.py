#!/usr/bin/env python3
"""
Aura Agent Demo — 完整 Agent 管线演示

Usage:
  python3 tests/agent_demo.py                  # 全部演示
  python3 tests/agent_demo.py --query           # 只查 AST
  python3 tests/agent_demo.py --fix             # 只演示自动修复
  python3 tests/agent_demo.py --transform       # 只演示变换
  python3 tests/agent_demo.py --serve           # 只演示 serve 协议
  python3 tests/agent_demo.py --hot-swap        # 只演示热替换
  python3 tests/agent_demo.py --typecheck       # 只演示类型检查
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

AURA = Path(__file__).resolve().parent.parent / "build" / "aura"


def run(cmd, input_data="", timeout=10):
    """Run aura with input, return (stdout, stderr, returncode)."""
    if not AURA.exists():
        print(f"  ERROR: {AURA} not found — run 'python3 build.py build' first")
        return "", "", 1
    try:
        r = subprocess.run(
            cmd, input=input_data, capture_output=True, text=True, timeout=timeout
        )
        return r.stdout.strip(), r.stderr.strip(), r.returncode
    except subprocess.TimeoutExpired:
        print(f"  ERROR: command timed out after {timeout}s: {' '.join(cmd)}")
        return "", "TIMEOUT", -1
    except FileNotFoundError as e:
        print(f"  ERROR: {e}")
        return "", str(e), -1


def run_serve_multi(commands, timeout=15):
    """Run multiple serve commands in a single --serve session.
    commands: list of (input_str, desc) tuples.
    Returns list of parsed JSON responses (or raw text if not JSON).
    """
    if not AURA.exists():
        print(f"  ERROR: {AURA} not found")
        return [{"status": "error"}]

    # Join all commands with newlines for a single session
    pipe_input = "\n".join(cmd for cmd, _ in commands) + "\n"

    try:
        r = subprocess.run(
            [str(AURA), "--serve"],
            input=pipe_input,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        # Parse each output line
        results = []
        for line in r.stdout.strip().split("\n"):
            if line:
                try:
                    results.append(json.loads(line))
                except json.JSONDecodeError:
                    results.append({"raw": line})
        return results
    except subprocess.TimeoutExpired:
        print(f"  ERROR: serve session timed out")
        return [{"status": "timeout"}]
    except FileNotFoundError as e:
        print(f"  ERROR: {e}")
        return [{"status": "error"}]


def section(title):
    """Print a section header."""
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print(f"{'=' * 60}")


def demo_query():
    """AST 查询演示"""
    section("AST Query (--query)")

    # Query for LiteralInt nodes
    code = "(+ 1 2)"
    stdout, _, rc = run([str(AURA), "--query", "(node-type LiteralInt)"], code)
    print(f"  Code:    {code}")
    print(f"  Query:   (node-type LiteralInt)")
    print(f"  Result:  {stdout[:80] if stdout else '(empty)'}")
    print(f"  Status:  {'OK' if rc == 0 else 'FAIL'}")

    # Query for Call nodes
    code = "(list 1 2 3)"
    stdout, _, rc = run([str(AURA), "--query", "(node-type Call)"], code)
    print(f"\n  Code:    {code}")
    print(f"  Query:   (node-type Call)")
    print(f"  Result:  {stdout[:80] if stdout else '(empty)'}")
    print(f"  Status:  {'OK' if rc == 0 else 'FAIL'}")


def demo_fix():
    """自动修复演示 (--serve + fix protocol)"""
    section("Auto-Fix via --serve")

    for code in ["(+ x 1)", "(+ 1 / 2)", "(undefined 42)"]:
        stdout, stderr, rc = run([str(AURA), "--serve"], code)
        print(f"  Input:   {code}")
        had_output = False
        for line in stdout.split("\n"):
            if line:
                try:
                    resp = json.loads(line)
                    status = resp.get("status", "?")
                    if status == "error":
                        msg = resp.get("msg", resp.get("value", ""))
                        print(f"    Error:  {msg}")
                        had_output = True
                    elif status == "fix":
                        patches = resp.get("patches", 0)
                        print(f"    Fix:    applied ({patches} patches)")
                        had_output = True
                    elif status == "fixed":
                        val = resp.get("value", "")
                        print(f"    Fixed:  {val}")
                        had_output = True
                    elif status == "ok":
                        val = resp.get("value", "")
                        print(f"    OK:     {val}")
                        had_output = True
                except json.JSONDecodeError:
                    pass
        if not had_output:
            print(f"    (no structured output)")
        if stderr:
            print(f"    stderr: {stderr[:60]}")


def demo_transform():
    """AST 变换演示"""
    section("AST Transform (--query-and-fix)")

    # Transform 1: Replace all LiteralInt with 99
    code = "(+ 1 2)"
    stdout, _, _ = run(
        [str(AURA), "--query-and-fix", "(node-type LiteralInt)", "(LiteralInt 99)"],
        code,
    )
    print(f"  Before:     {code}")
    print(f"  Query:      (node-type LiteralInt)")
    print(f"  Replace:    (LiteralInt 99)")
    print(f"  After:      {stdout.strip()[:80]}")

    # Transform 2: Replace + with *
    code = "(+ 1 2 3)"
    stdout, _, _ = run(
        [str(AURA), "--query-and-fix", "(call-callee-name +)", "(call-callee-name *)"],
        code,
    )
    print(f"\n  Before:     {code}")
    print(f"  Query:      (call-callee-name +)")
    print(f"  Replace:    (call-callee-name *)")
    print(f"  After:      {stdout.strip()[:80]}")


def demo_typecheck():
    """类型检查演示"""
    section("Type Check with Positions")

    cases = [
        ('(+ 1 "a")', "Type coercion with position"),
        ("x", "Unbound variable"),
        ('(let ((x 10)) (+ x "hello"))', "Type error in let body"),
        ("42", "Literal"),
        ("(+ 1 2)", "Simple addition"),
    ]

    for code, desc in cases:
        stdout, stderr, rc = run([str(AURA), "--typecheck"], code)
        # Show diagnostic line if present
        type_line = ""
        diagnostic = ""
        for line in stdout.split("\n"):
            if line.startswith("type:"):
                type_line = line
            elif line.strip().startswith("[") and "error" in line:
                diagnostic = line.strip()

        print(f"  {desc}:")
        print(f"    code: {code}")
        if type_line:
            print(f"    {type_line}")
        if diagnostic:
            print(f"    {diagnostic}")
        if stderr:
            print(f"    stderr: {stderr[:60]}")
        print(f"    status: {'OK' if rc == 0 else 'diagnostics found'}")


def demo_serve():
    """Serve JSON 协议演示 (single session, multi-line input)"""
    section("Serve JSON Protocol (single session)")

    commands = [
        (
            '{"cmd":"define","code":"(define double (lambda (x) (* x 2)))","name":"double"}',
            "Define double",
        ),
        ('{"cmd":"exec","code":"(double 5)"}', "Exec double(5)"),
        ('{"cmd":"exec","code":"(double (double 3))"}', "Exec double(double(3))"),
        ('{"cmd":"query","code":"double"}', "Query double"),
    ]

    results = run_serve_multi(commands)
    for (_, desc), resp in zip(commands, results):
        status = resp.get("status", resp.get("raw", "?"))
        value = resp.get("value", resp.get("name", ""))
        if isinstance(status, str) and status == "ok":
            print(f"  ✓ {desc}: {value}")
        elif isinstance(status, str) and status == "error":
            print(f"  ✗ {desc}: error — {resp.get('msg', '')}")
        else:
            print(f"  → {desc}: {status} {value}")


def demo_hot_swap():
    """热替换演示 (single session)"""
    section("Hot Swap (redefine in single session)")

    commands = [
        (
            '{"cmd":"define","code":"(define mul2 (lambda (x y) (* x y)))","name":"mul2"}',
            "Define mul2 = *",
        ),
        ('{"cmd":"exec","code":"(mul2 3 4)"}', "mul2(3,4) = ?"),
        (
            '{"cmd":"redefine","code":"(define mul2 (lambda (x y) (+ x y)))","name":"mul2"}',
            "Redefine mul2 → +",
        ),
        ('{"cmd":"exec","code":"(mul2 3 4)"}', "mul2(3,4) = ?"),
    ]

    results = run_serve_multi(commands)
    for (_, desc), resp in zip(commands, results):
        status = resp.get("status", resp.get("raw", "?"))
        value = resp.get("value", resp.get("name", ""))
        if status == "ok":
            print(f"  ✓ {desc}: {value}")
        elif status == "error":
            print(f"  ✗ {desc}: error — {resp.get('msg', '')}")
        else:
            print(f"  → {desc}: {status} {value}")


def demo_all():
    """完整演示"""
    print(f"\n{'=' * 60}")
    print(f"  Aura Agent Demo - Full Pipeline")
    print(f"  Binary: {AURA}")
    print(f"{'=' * 60}")

    if not AURA.exists():
        print(f"\n  ERROR: {AURA} not found!")
        print(f"  Run 'python3 build.py build' first.")
        return

    try:
        demo_query()
    except Exception as e:
        print(f"  [query demo failed: {e}]")

    try:
        demo_transform()
    except Exception as e:
        print(f"  [transform demo failed: {e}]")

    try:
        demo_fix()
    except Exception as e:
        print(f"  [fix demo failed: {e}]")

    try:
        demo_typecheck()
    except Exception as e:
        print(f"  [typecheck demo failed: {e}]")

    try:
        demo_serve()
    except Exception as e:
        print(f"  [serve demo failed: {e}]")

    try:
        demo_hot_swap()
    except Exception as e:
        print(f"  [hot-swap demo failed: {e}]")

    print(f"\n{'=' * 60}")
    print(f"  Demo complete!")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    modes = {
        "all": demo_all,
        "query": demo_query,
        "fix": demo_fix,
        "transform": demo_transform,
        "serve": demo_serve,
        "hot-swap": demo_hot_swap,
        "typecheck": demo_typecheck,
    }
    func = modes.get(mode, demo_all)
    try:
        func()
    except Exception as e:
        print(f"\nFATAL: {e}")
        sys.exit(1)
