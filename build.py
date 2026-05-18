#!/usr/bin/env python3
"""
Aura — 统一构建/测试入口

Usage:
  ./build.py build            # CMake 构建
  ./build.py test [suite]     # 运行测试
  ./build.py check            # 构建 + 全部测试
  ./build.py clean            # 清理构建产物
  ./build.py list             # 列出测试套件
  ./build.py demo             # 运行 Agent 管线演示

Test suites:
  unit        C++ 单元测试 (61 cases)
  integ       端到端管线测试 (.aura)
  typecheck   类型检查测试
  bench       Benchmark 基线 + 回归检测
  smoke       快速冒烟测试
  all         全部测试 (默认)
"""

import subprocess
import sys
import os
import json
import time
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
AURA = BUILD / "aura"
TEST_BIN = BUILD / "test_ir"
BENCH = ROOT / "tests" / "benchmark.py"

# ── Colors ──────────────────────────────────────────────────────
G = "\033[32m"  # green
Y = "\033[33m"  # yellow
R = "\033[31m"  # red
B = "\033[34m"  # blue
N = "\033[0m"   # reset

def ok(msg):   print(f"  {G}✓{N} {msg}")
def fail(msg): print(f"  {R}✗{N} {msg}")
def warn(msg): print(f"  {Y}!{N} {msg}")
def info(msg): print(f"  {B}→{N} {msg}")


# ═══════════════════════════════════════════════════════════════
# Build
# ═══════════════════════════════════════════════════════════════

def cmd_build():
    """CMake 构建"""
    print(f"{B}═══ Build ═══{N}")
    BUILD.mkdir(parents=True, exist_ok=True)
    r = run(["cmake", "-B", str(BUILD)], cwd=ROOT)
    if r != 0: return r
    r = run(["cmake", "--build", str(BUILD), "-j$(nproc)"], cwd=ROOT)
    if r == 0: ok("build OK")
    else:      fail("build failed")
    return r


def cmd_clean():
    """清理构建产物"""
    print(f"{B}═══ Clean ═══{N}")
    if BUILD.exists():
        run(["cmake", "--build", str(BUILD), "--target", "clean"], cwd=ROOT)
        # Also remove directory
        import shutil
        shutil.rmtree(BUILD)
        ok(f"removed {BUILD}")
    else:
        info("nothing to clean")
    return 0


# ═══════════════════════════════════════════════════════════════
# Unit tests (C++)
# ═══════════════════════════════════════════════════════════════

def test_unit():
    """C++ 单元测试 — test_ir (61 cases)"""
    print(f"{B}═══ Unit tests ═══{N}")
    if not TEST_BIN.exists():
        fail(f"{TEST_BIN} not found — run 'build' first")
        return 1
    
    start = time.time()
    r = subprocess.run([str(TEST_BIN)], capture_output=True, text=True)
    elapsed = time.time() - start
    
    # Parse results
    lines = r.stdout.strip().split("\n")
    passed = failed = 0
    for line in lines:
        if "passed" in line.lower():
            ok(line.strip())
        elif "FAIL" in line:
            fail(line.strip())
            failed += 1
        elif "passed/failed" in line and "Memory" not in line:
            pass  # These are the subsection summaries
    
    print(f"  Unit tests: {elapsed:.2f}s")
    if r.returncode == 0 and failed == 0:
        ok("all unit tests passed")
    else:
        fail(f"unit tests: returncode={r.returncode}, failures={failed}")
    return r.returncode


# ═══════════════════════════════════════════════════════════════
# Integration tests (.aura files)
# ═══════════════════════════════════════════════════════════════

@dataclass
class IntegCase:
    name: str
    code: str
    pipeline: str          # "eval" | "ir" | "typecheck" | "serve"
    expected: str = ""     # expected stdout (substring match)
    expected_err: str = "" # expected stderr (substring match)
    expected_status: int = 0  # expected exit code


INTEG_TESTS = [
    # ── Eval pipeline ────────────────────────────────────────
    IntegCase("literal_int", "42", "eval", expected="42"),
    IntegCase("literal_neg", "-5", "eval", expected="-5"),
    IntegCase("add", "(+ 1 2)", "eval", expected="3"),
    IntegCase("sub", "(- 10 3)", "eval", expected="7"),
    IntegCase("mul", "(* 6 7)", "eval", expected="42"),
    IntegCase("div", "(/ 100 5)", "eval", expected="20"),
    IntegCase("let_simple", "(let ((x 10)) x)", "eval", expected="10"),
    IntegCase("lambda", "((lambda (x) (* x 2)) 5)", "eval", expected="10"),
    IntegCase("closure", "(let ((f (lambda (x) (+ x 1)))) (f 41))", "eval", expected="42"),
    IntegCase("if_true", "(if 1 42 0)", "eval", expected="42"),
    IntegCase("if_false", "(if 0 42 0)", "eval", expected="0"),
    IntegCase("fact_5", "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))", "eval", expected="120"),
    IntegCase("string", '(string-append "a" "b")', "eval", expected="ab"),
    IntegCase("pair", "(car (cons 1 2))", "eval", expected="1"),

    # ── IR pipeline ──────────────────────────────────────────
    IntegCase("ir_add", "(+ 1 2)", "ir", expected="3"),
    IntegCase("ir_lambda", "((lambda (x) (* x 2)) 5)", "ir", expected="10"),
    IntegCase("ir_fact", "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))", "ir", expected="120"),
    IntegCase("ir_fib", "(letrec ((fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))) (fib 10))", "ir", expected="55"),

    # ── Typecheck pipeline ───────────────────────────────────
    IntegCase("tc_int", "42", "typecheck", expected="Int"),
    IntegCase("tc_add", "(+ 1 2)", "typecheck", expected="Int"),
    IntegCase("tc_str", '"hello"', "typecheck", expected="String"),
    IntegCase("tc_type_of", "(type-of 42)", "typecheck", expected="Type"),
    IntegCase("tc_type_query", '(type? 42 "Int")', "typecheck", expected="Bool"),
    IntegCase("tc_occurrence", '(let ((x "hello")) (if (string? x) x "fallback"))', "typecheck", expected="String", expected_status=0),
    IntegCase("tc_coercion", '(+ "42" 1)', "typecheck", expected="Int", expected_status=0),

    # ── Gradual coercion runtime ─────────────────────────────
    IntegCase("coerce_arith", '(+ 1 "2")', "eval", expected="3"),
    IntegCase("coerce_strlen", "(string-length 12345)", "eval", expected="5"),

    # ── Serve JSON command protocol ─────────────────────────
    IntegCase("serve_define",
              '{"cmd":"define","code":"(define add (lambda (x y) (+ x y)))","name":"add"}\n'
              '{"cmd":"exec","code":"(add 1 2)"}',
              "serve", expected="\"3\""),
    IntegCase("serve_define_redefine",
              '{"cmd":"define","code":"(define mul (lambda (x y) (* x y)))","name":"mul"}\n'
              '{"cmd":"exec","code":"(mul 3 4)"}\n'
              '{"cmd":"redefine","code":"(define mul (lambda (x y) (+ x y)))","name":"mul"}\n'
              '{"cmd":"exec","code":"(mul 3 4)"}',
              "serve", expected="\"7\""),
    IntegCase("serve_plain_sexpr",
              '(+ 1 2)',
              "serve", expected="\"3\""),
    IntegCase("serve_unknown_cmd",
              '{"cmd":"noop","code":"(+ 1 2)"}',
              "serve", expected="unknown command"),

    # ── Error cases ──────────────────────────────────────────
    IntegCase("err_unbound", "x", "eval", expected_err="unbound variable", expected_status=1),
    IntegCase("err_type", '(+ 1 "a")', "typecheck", expected_err="", expected_status=0),

    # ── Vector operations ──────────────────────────────────
    IntegCase("vector_basic", "(vector 1 2 3)", "eval", expected="vector"),
    IntegCase("vector_ref", "(vector-ref (vector 10 20 30) 1)", "eval", expected="20"),
    IntegCase("vector_length", "(vector-length (vector 1 2 3))", "eval", expected="3"),
    IntegCase("vector_set", "(begin (vector-set! (vector 1 2 3) 0 99) 42)", "eval", expected="42"),
    IntegCase("tc_vector", "(vector-length (vector (list 1)))", "typecheck", expected="Int"),
    IntegCase("tc_macro_def", "(defmacro (twice x) (+ x x))", "typecheck", expected="(Any -> Int)"),
    IntegCase("vector_pred", "(vector? (vector 1 2 3))", "eval", expected="#t"),
    IntegCase("make_vector", "(vector-length (make-vector 5 42))", "eval", expected="5"),
    IntegCase("list_to_vector", "(vector-length (list->vector (list 1 2 3)))", "eval", expected="3"),
    IntegCase("vector_to_list", "(length (vector->list (vector 1 2 3)))", "eval", expected="3"),

# ── List operations ─────────────────────────────────────
    IntegCase("list_basic", "(list 1 2 3)", "eval", expected="(1 2 3)"),
    IntegCase("list_length", "(length (list 1 2 3))", "eval", expected="3"),
    IntegCase("list_ref", "(list-ref (list 10 20 30) 1)", "eval", expected="20"),
    IntegCase("list_reverse", "(length (reverse (list 1 2 3)))", "eval", expected="3"),
    IntegCase("list_append", "(length (append (list 1 2) (list 3 4)))", "eval", expected="4"),
    IntegCase("list_member_found", "(member 2 (list 1 2 3))", "eval", expected="(2 3)"),
    IntegCase("list_member_not_found", "(member 99 (list 1 2 3))", "eval", expected="0"),
    IntegCase("map_length", "(length (map (lambda (x) (* x 2)) (list 1 2 3)))", "eval", expected="3"),
    IntegCase("filter_count", "(length (filter (lambda (x) (> x 2)) (list 1 2 3 4 5)))", "eval", expected="3"),
    IntegCase("nested_list", "(car (car (list (list 1 2) (list 3 4))))", "eval", expected="1"),

    # ── Type checker edge cases ────────────────────────────
    IntegCase("tc_list", "(list 1 2 3)", "typecheck", expected="Any", expected_status=0),
    IntegCase("tc_map", "(map (lambda (x) (* x 2)) (list 1 2))", "typecheck", expected="Int", expected_status=0),
    IntegCase("tc_filter", "(filter (lambda (x) (> x 2)) (list 1 2))", "typecheck", expected="Bool", expected_status=0),
    IntegCase("tc_string_compare", '(string=? "a" "a")', "typecheck", expected="Bool"),
    IntegCase("tc_append", '(string-append "a" "b")', "typecheck", expected="String"),
    IntegCase("tc_pair", "(cons 1 2)", "typecheck", expected="Any"),
    IntegCase("tc_let_lambda", "(let ((f (lambda (x) (+ x 1)))) (f 41))", "typecheck", expected="Int"),

    # ── Error recovery ─────────────────────────────────────
    IntegCase("err_div_zero", "(/ 1 0)", "eval", expected_err="", expected_status=0),
    IntegCase("err_unbound_var", "nonexistent", "eval", expected_err="unbound variable", expected_status=1),
    IntegCase("err_wrong_arg_type", '(/ "a" 1)', "typecheck", expected_err="", expected_status=0),
    IntegCase("err_arity", "(+ 1)", "typecheck", expected="Any", expected_status=0),

    # ─── IR pipeline edge cases ────────────────────────────
    IntegCase("ir_fold_arith", "(+ (* 2 3) 4)", "ir", expected="10"),
    IntegCase("ir_let_arith", "(let ((x (+ 1 2))) (* x 3))", "ir", expected="9"),
    IntegCase("ir_nested_arith", "(+ (* 2 3) (/ 10 2))", "ir", expected="11"),

    # ── Apply / variadic ──────────────────────────────────────
    IntegCase("apply_add", "(apply + (list 1 2 3))", "eval", expected="6"),
    IntegCase("apply_str", '(apply string-append (list "hello " "world"))', "eval", expected="hello world"),
    IntegCase("variadic_lambda", "(apply (lambda (x . rest) (cons x rest)) (list 1 2 3))", "eval", expected="(1 2 3)"),

    # ── Char operations ──────────────────────────────────────
    IntegCase("char_eq", "(char=? 65 65)", "eval", expected="#t"),
    IntegCase("char_lt", "(char<? 65 66)", "eval", expected="#t"),
    IntegCase("char_alpha", "(char-alphabetic? 65)", "eval", expected="#t"),
    IntegCase("char_numeric", "(char-numeric? 48)", "eval", expected="#t"),
    IntegCase("char_whitespace", "(char-whitespace? 32)", "eval", expected="#t"),
    IntegCase("char_upcase", "(char-upcase 97)", "eval", expected="65"),
    IntegCase("char_downcase", "(char-downcase 65)", "eval", expected="97"),
    IntegCase("char_to_int", "(char->integer 65)", "eval", expected="65"),
    IntegCase("int_to_char", "(integer->char 65)", "eval", expected="65"),

    # ── String operations ────────────────────────────────────
    IntegCase("str_to_list", "(car (string->list \"ABC\"))", "eval", expected="65"),
    IntegCase("list_to_str", '(list->string (list 65 66 67))', "eval", expected="ABC"),
    IntegCase("str_join", '(string-join (list "a" "b" "c") ",")', "eval", expected="a,b,c"),

    # ── Format ────────────────────────────────────────────────
    IntegCase("format_basic", '(format "~a = ~a" "x" 42)', "eval", expected="x = 42"),
    IntegCase("format_write", '(format "~s" "hello")', "eval", expected='"hello"'),

    # ── string->number whitespace ─────────────────────────────
    IntegCase("str_num_trim", '(string->number " 42 ")', "eval", expected="42"),
    IntegCase("str_num_tab", '(string->number "\t-7\n")', "eval", expected="-7"),
    IntegCase("str_num_only_space", '(string->number "  ")', "eval", expected="#f"),
]


def test_integ():
    """端到端管线测试 — eval / ir / typecheck / serve"""
    print(f"{B}═══ Integration tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found — run 'build' first")
        return 1

    flags = {"eval": [], "ir": ["--ir"], "typecheck": ["--typecheck"], "serve": ["--serve"]}
    passed = failed = 0

    for tc in INTEG_TESTS:
        args = [str(AURA)] + flags.get(tc.pipeline, [])

        # For serve pipeline, code may contain embedded newlines (multi-line input)
        if tc.pipeline == "serve":
            pipe_input = tc.code
        else:
            pipe_input = tc.code + "\n"

        r = subprocess.run(
            args, input=pipe_input,
            capture_output=True, text=True, timeout=30
        )

        ok_case = True
        issues = []

        # Check exit status
        if r.returncode != tc.expected_status:
            ok_case = False
            issues.append(f"exit_code={r.returncode} (expected {tc.expected_status})")

        # Check expected stdout
        stdout = r.stdout.strip()
        stderr = r.stderr.strip()

        # For serve pipeline, check the LAST line of stdout for the expected value
        check_stdout = stdout
        if tc.pipeline == "serve":
            # Each serve response is one JSON line; use last line for matching
            lines = stdout.split("\n")
            check_stdout = lines[-1] if lines else stdout

        if tc.expected and tc.expected not in check_stdout:
            ok_case = False
            issues.append(f"expected '{tc.expected}' in stdout, got: {stdout[:80]}...")

        if tc.expected_err:
            # Check both stdout and stderr for error message
            combined = stdout + "\n" + stderr
            if tc.expected_err not in combined:
                ok_case = False
                issues.append(f"expected error '{tc.expected_err}' not found")

        if ok_case:
            ok(f"[{tc.pipeline:10s}] {tc.name}")
            passed += 1
        else:
            fail(f"[{tc.pipeline:10s}] {tc.name} — {'; '.join(issues)}")
            failed += 1

    print(f"  Integration: {passed}/{passed+failed} passed")
    return 1 if failed > 0 else 0


# ═══════════════════════════════════════════════════════════════
# Typecheck tests (focused type system tests)
# ═══════════════════════════════════════════════════════════════

def test_typecheck():
    """类型检查专项测试"""
    print(f"{B}═══ Typecheck tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    cases = [
        # (name, code, expected_type, should_pass)
        ("int_literal",   "42",                          "Int",    True),
        ("str_literal",   '"hi"',                        "String", True),
        ("add",           "(+ 1 2)",                     "Int",    True),
        ("string_append", '(string-append "a" "b")',     "String", True),
        ("lambda",        "(lambda (x) x)",              "->",     True),
        ("type_of",       "(type-of 42)",                "Type",   True),
        ("type_query",    '(type? 42 "Int")',            "Bool",   True),
        ("occurrence",    '(let ((x "hi")) (if (string? x) x "fallback"))', "String", True),
        ("coercion_note", '(+ "42" 1)',                  "Int",    True),

        # Error cases: should still typecheck (produce diagnostics but infer a type)
        ("wrong_arity",   "(+ 1)",                       "Int",    True),
    ]

    passed = failed = 0
    for name, code, exp_type, should_pass in cases:
        r = subprocess.run([str(AURA), "--typecheck"], input=code + "\n",
                           capture_output=True, text=True, timeout=10)
        stdout = r.stdout.strip()

        has_type = f"type: {exp_type}" in stdout or f"type:" in stdout
        # Check the type line contains the expected type
        type_ok = False
        for line in stdout.split("\n"):
            if line.startswith("type:"):
                if exp_type in line:
                    type_ok = True
                break

        if type_ok:
            ok(f"{name:25s} → {exp_type}")
            passed += 1
        else:
            fail(f"{name:25s} expected '{exp_type}', got: {stdout[:80]}")
            failed += 1

    print(f"  Typecheck: {passed}/{passed+failed} passed")
    return 1 if failed > 0 else 0


# ═══════════════════════════════════════════════════════════════
# Benchmark
# ═══════════════════════════════════════════════════════════════

def test_bench():
    """Benchmark 基线 + 回归检测"""
    print(f"{B}═══ Benchmark ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1
    return run([sys.executable, str(BENCH)])


# ═══════════════════════════════════════════════════════════════
# Smoke tests (quick sanity)
# ═══════════════════════════════════════════════════════════════

SMOKE_TESTS = [
    ("basic_eval",      "echo 42 | build/aura",                     "42"),
    ("basic_add",       "echo '(+ 1 2)' | build/aura",            "3"),
    ("basic_ir",        "echo '(+ 1 2)' | build/aura --ir",        "3"),
    ("basic_typecheck", "echo 42 | build/aura --typecheck",        "Int"),
    ("basic_serve",     "printf '(+ 1 2)' | build/aura --serve",   'status'),
]


def test_smoke():
    """快速冒烟测试"""
    print(f"{B}═══ Smoke tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    passed = failed = 0
    for name, cmd, expected in SMOKE_TESTS:
        r = subprocess.run(
            ["bash", "-c", f"cd {ROOT} && {cmd}"],
            capture_output=True, text=True, timeout=15
        )
        combined = r.stdout + r.stderr
        if expected in combined:
            ok(f"{name:20s} → {expected}")
            passed += 1
        else:
            fail(f"{name:20s} expected '{expected}', got '{combined[:60]}'")
            failed += 1

    print(f"  Smoke: {passed}/{passed+failed} passed")
    return 1 if failed > 0 else 0


# ═══════════════════════════════════════════════════════════════
# Mutation tests (变异循环)
# ═══════════════════════════════════════════════════════════════

def test_mutation():
    """Agent 变异循环 — mutation loop 功能验证"""
    print(f"{B}═══ Mutation tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    r = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "mutation_loop.py"),
         "--demo"],
        capture_output=True, text=True, timeout=30
    )
    print(r.stdout)
    if r.returncode != 0:
        print(r.stderr)
        fail("mutation demo failed")
        return 1
    ok("mutation: demo OK")

    r2 = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "mutation_loop.py"),
         "--list"],
        capture_output=True, text=True, timeout=15
    )
    print(r2.stdout)
    if r2.returncode != 0:
        print(r2.stderr)
        fail("mutation list failed")
        return 1
    ok("mutation: list OK")

    r3 = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "mutation_loop.py"),
         str(ROOT / "tests" / "fixtures" / "basic_add.aura"), "--fast"],
        capture_output=True, text=True, timeout=30
    )
    print(r3.stdout)
    if r3.returncode != 0:
        print(r3.stderr)
        fail("mutation single-pass failed")
        return 1
    ok("mutation: single-pass OK")
    return 0


# ═══════════════════════════════════════════════════════════════
# Agent demo
# ═══════════════════════════════════════════════════════════════

def test_demo():
    """Agent demo — full pipeline"""
    print(f"{B}═══ Agent Demo ═══{N}")
    r = subprocess.run([sys.executable, str(ROOT / "tests" / "agent_demo.py")])
    if r.returncode == 0:
        ok("agent demo passed")
    else:
        fail("agent demo failed")
    return r.returncode


def test_ai_agent_demo():
    """AI Agent 端到端演示 — 6 场景工具链"""
    print(f"{B}═══ AI Agent Demo ═══{N}")
    r = subprocess.run([sys.executable, str(ROOT / "tests" / "ai_agent_demo.py")], timeout=120)
    if r.returncode == 0:
        ok("ai agent demo passed")
    else:
        fail("ai agent demo failed")
    return r.returncode


# ═══════════════════════════════════════════════════════════════
# Runners
# ═══════════════════════════════════════════════════════════════

def test_bash():
    """Bash 回归测试 — run-tests.sh (76+ cases)"""
    print(f"{B}═══ Bash regression tests ═══{N}")
    runner = ROOT / "tests" / "run-tests.sh"
    if not runner.exists():
        fail(f"{runner} not found")
        return 1
    r = subprocess.run(
        ["bash", str(runner)],
        env={**os.environ, "AURA": str(AURA)},
        capture_output=True, text=True, timeout=120
    )

SUITES = {
    "unit":      test_unit,
    "integ":     test_integ,
    "typecheck": test_typecheck,
    "bench":     test_bench,
    "smoke":     test_smoke,
    "mutation":  test_mutation,
    "demo":      test_demo,
    "ai":        test_ai_agent_demo,
    "bash":      test_bash,
}


def run(cmd, **kwargs):
    """Run command and return exit code."""
    result = subprocess.run(cmd, **kwargs)
    return result.returncode


def cmd_test(suite_names: list[str]):
    """Run test suites."""
    if not suite_names or "all" in suite_names:
        suite_names = list(SUITES.keys())

    results = {}
    for name in suite_names:
        if name in SUITES:
            results[name] = SUITES[name]()
        else:
            warn(f"unknown suite '{name}' (use: {', '.join(SUITES.keys())})")

    print(f"\n{'═'*50}")
    all_ok = all(v == 0 for v in results.values())
    total = len(results)
    good = sum(1 for v in results.values() if v == 0)
    bad = total - good
    if bad == 0:
        print(f"{G}All {total} test suites passed{N}")
    else:
        print(f"{R}{bad}/{total} test suites failed{N}")
    return 1 if not all_ok else 0


def cmd_list():
    """列出测试套件"""
    print(f"{B}Available test suites:{N}")
    for name, func in SUITES.items():
        print(f"  {name:12s} {func.__doc__}")
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0

    cmd = sys.argv[1]
    args = sys.argv[2:]

    commands = {
        "build": cmd_build,
        "clean": cmd_clean,
        "check": lambda: (cmd_build(), cmd_test([]))[1],
        "test":  lambda: cmd_test(args or ["all"]),
        "list":  cmd_list,
        "demo":  test_demo,
    }

    if cmd in commands:
        rc = commands[cmd]()
    else:
        warn(f"unknown command '{cmd}'")
        print(__doc__.strip())
        rc = 1

    sys.exit(rc)


if __name__ == "__main__":
    main()
