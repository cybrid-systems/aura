#!/usr/bin/env python3
"""Aura AI Agent — --serve 协议演示

连接到 Aura 的 --serve 模式，展示 AI Agent 可以使用的全部语言能力。

用法:
  python3 tests/ai_agent_serve.py

原理:
  AI Agent (LLM) 通过 JSON Lines 协议与 Aura 交互:
  1. LLM 生成 Aura 代码
  2. 通过 {"cmd":"exec","code":"..."} 发送
  3. Aura 执行并返回 JSON 结果
  4. 如果出错，LLM 分析错误并修正代码
  5. 重复直到成功或放弃

交互示例:
  → {"cmd":"exec","code":"(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))"}
  ← {"status":"ok","value":"<closure>"}

  → {"cmd":"exec","code":"(fib 10)"}
  ← {"status":"ok","value":"55"}
"""

import subprocess
import json
import sys
import os

AURA = os.environ.get("AURA_BIN", "./build/aura")


class AuraSession:
    """持久化 --serve 会话"""

    def __init__(self):
        self.proc = subprocess.Popen(
            [AURA, "--serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True, bufsize=1,
        )

    def exec(self, code: str) -> dict:
        """执行代码，返回 JSON 结果"""
        self.proc.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        self.proc.stdin.flush()
        for _ in range(20):
            line = self.proc.stdout.readline()
            if line.startswith("{"):
                return json.loads(line)
        return {"status": "timeout"}

    def define(self, code: str) -> dict:
        """定义函数（在会话中持久化）"""
        return self.exec(code)

    def close(self):
        self.proc.terminate()
        self.proc.wait()


# ── 工具函数：测试套件 ────────────────────────────────────
_PASS = 0
_TOTAL = 0


def check(session, desc: str, code: str, expected: str) -> bool:
    global _PASS, _TOTAL
    _TOTAL += 1
    r = session.exec(code)
    actual = r.get("value", r.get("msg", ""))
    ok = actual == expected
    if ok:
        _PASS += 1
        print(f"  ✅ {desc}")
    else:
        print(f"  ❌ {desc}")
        print(f"     code: {code}")
        print(f"     want: {expected}")
        print(f"     got:  {actual}")
    return ok


def show(name: str):
    print(f"\n── {name} ──")


# ── 演示 ────────────────────────────────────────────────────
def demo():
    global _PASS, _TOTAL
    session = AuraSession()

    print("=" * 60)
    print("Aura --serve 协议演示")
    print("=" * 60)

    # 1. 算术
    show("算术")
    check(session, "(+ 1 2)", "(+ 1 2)", "3")
    check(session, "变参 (+ 1 2 3)", "(+ 1 2 3)", "6")
    check(session, "float (+ 1.5 2.5)", "(+ 1.5 2.5)", "4")
    check(session, "modulo", "(modulo 10 3)", "1")
    check(session, "gcd", "(gcd 12 8)", "4")
    check(session, "abs", "(abs -5)", "5")

    # 2. 顺序对/列表
    show("列表")
    check(session, "cons", "(cons 1 2)", "(1 . 2)")
    check(session, "list", "(list 1 2 3)", "(1 2 3)")
    check(session, "map", "(map (lambda (x) (* x 2)) (list 1 2 3))", "(2 4 6)")
    check(session, "filter", "(filter (lambda (x) (> x 2)) (list 1 2 3 4))", "(3 4)")
    check(session, "foldl", "(foldl (lambda (a x) (+ a x)) 0 (list 1 2 3))", "6")

    # 3. 函数定义 + 调用
    show("函数")
    session.define("(define (square x) (* x x))")
    check(session, "square(5)", "(square 5)", "25")

    session.define("(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))")
    check(session, "fib(10)", "(fib 10)", "55")
    check(session, "fib(20)", "(fib 20)", "6765")

    # 4. 字符串
    show("字符串")
    check(session, "string-append", '(string-append "a" "b" "c")', '"abc"')
    check(session, "string-length", '(string-length "hello")', "5")
    check(session, "string-ref", '(string-ref "hello" 0)', "104")
    check(session, "string->number", '(string->number "42")', "42")

    # 5. 匹配 + 条件
    show("匹配/条件")
    check(session, "match 解构",
          "(match (list 1 2 3) ((list a b c) (+ a b c)) (_ 0))", "6")
    check(session, "match empty",
          "(match () (() \"empty\") (_ \"not\"))", "\"empty\"")
    check(session, "cond",
          "(cond ((< 3 5) \"less\") (else \"other\"))", "\"less\"")

    # 6. 类型 + 谓词
    show("类型")
    check(session, "integer?", "(integer? 42)", "#t")
    check(session, "string?", '(string? "hello")', "#t")
    check(session, "null?", "(null? (list))", "#t")
    check(session, "null? (cons)", "(null? (cons 1 2))", "#f")
    check(session, "procedure?", "(procedure? (lambda (x) x))", "#t")

    # 7. 结构体
    show("结构体")
    session.define("(import \"std/struct\")")
    session.define("(define-struct point (x y))")
    check(session, "make-point", "(point? (make-point 10 20))", "#t")
    check(session, "point-x", "(point-x (make-point 10 20))", "10")

    # 8. 错误处理
    show("错误处理")
    check(session, "除零", "(/ 1 0)", "0")
    # display 输出到 stderr (从 --serve 模式), JSON 协议不受影响
    check(session, "display", "(display 42)", "1")

    # ── 结果 ──
    print(f"\n{'=' * 60}")
    print(f"结果: {_PASS}/{_TOTAL} 通过 ({100 * _PASS // _TOTAL}%)")

    # ── 协议文档 ──
    print(f"\n{'=' * 60}")
    print("--serve 协议参考")
    print(f"{'=' * 60}")
    print("""
  AI Agent 通过 JSON Lines 与 Aura 交互。

  命令格式:
    {"cmd":"exec","code":"Aura代码"}

  响应格式:
    {"status":"ok","value":"结果"}
    {"status":"error","msg":"错误信息"}

  关键: 函数定义通过 define 在会话中持久化。
  跨请求的状态（define、import）保留在求值器中。

  exec 使用树遍历求值器，支持全部语言特性
  （字符串、匹配、列表操作、闭包等）。
""")

    session.close()
    return _PASS == _TOTAL


if __name__ == "__main__":
    ok = demo()
    sys.exit(0 if ok else 1)
