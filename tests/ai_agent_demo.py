#!/usr/bin/env python3
"""Aura AI Agent 端到端演示

演示 AI Agent 如何通过 Aura 工具链自动修复代码错误。
模拟 LLM → tool call 循环，展示 OBSERVE → DIAGNOSE → ACT → VERIFY 四步工作流。

注意: string 操作（string-append/number->string 等）用树遍历器（defualt mode），
IR 管线只支持数值/闭包/递归。知道各求值路径的限制也是 Agent 需要掌握的。
"""

import json
import os
import subprocess
import sys

AURA = os.environ.get("AURA_BIN", "./build/aura")

# ═══════════════════════════════════════════════════════════════
# Tool 封装
# ═══════════════════════════════════════════════════════════════


def aura_query(code, query_expr):
    r = subprocess.run(
        [AURA, "--query", query_expr],
        input=code,
        capture_output=True,
        text=True,
        timeout=10,
    )
    return (r.stdout + r.stderr).strip()


def aura_query_and_fix(code, match, replace):
    r = subprocess.run(
        [AURA, "--query-and-fix", match, replace],
        input=code,
        capture_output=True,
        text=True,
        timeout=10,
    )
    return (r.stdout + r.stderr).strip()


def aura_typecheck(code):
    r = subprocess.run(
        [AURA, "--typecheck"], input=code, capture_output=True, text=True, timeout=10
    )
    return (r.stdout + r.stderr).strip()


def aura_ir(code):
    """IR 管线 — 数值/闭包/递归（不支持 string）"""
    r = subprocess.run(
        [AURA, "--ir"], input=code, capture_output=True, text=True, timeout=10
    )
    return r.stdout.strip()


def aura_eval(code):
    """树遍历器 — 完整原语集（含 string 操作）"""
    r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
    return r.stdout.strip()


def aura_cache(code, path):
    r = subprocess.run(
        [AURA, "--cache", path], input=code, capture_output=True, text=True, timeout=10
    )
    return (r.stdout + r.stderr).strip()


# ═══════════════════════════════════════════════════════════════
# 演示场景
# ═══════════════════════════════════════════════════════════════


def demo_type_error_fix():
    """场景 1: 类型错误自动修复"""
    print("\n" + "═" * 60)
    print("📦 场景 1: 类型错误自动修复 — coercion from String to Int")
    print("═" * 60)

    buggy_code = '(+ "42" 1)'

    # Step 1: OBSERVE — 类型检查发现错误
    print("\n[OBSERVE] aura --typecheck")
    tc_result = aura_typecheck(buggy_code)
    print(f"  输入: {buggy_code}")
    print(f"  诊断: {tc_result}")

    # Step 2: DIAGNOSE — 分析错误
    print("\n[DIAGNOSE] 分析错误")
    print(f"  → + : (-> Int Int Int)，参数 1 是 String，触发 coercion")
    print(f"  → 系统检测到类型错误并提供位置信息 1:2")

    # Step 3: ACT — 正确修复（用纯 Int 参数调用 +）
    print(f"\n[ACT] 正确调用: (+ 42 1)")

    # Step 4: VERIFY — 验证修复
    print("\n[VERIFY] aura --typecheck")
    tc_result = aura_typecheck("(+ 42 1)")
    print(f"  → {tc_result}")

    print("\n[VERIFY] aura --ir")
    ir_result = aura_ir("(+ 42 1)")
    print(f"  → {ir_result}")

    ok = "Int" in tc_result and ir_result == "43"
    print(f"\n  {'✅' if ok else '❌'} 类型正确: Int, IR 结果: {ir_result}")
    return ok


def demo_occurrence_typing():
    """场景 2: Occurrence Typing — if 分支类型细化"""
    print("\n" + "═" * 60)
    print("📦 场景 2: Occurrence Typing — if 分支类型细化")
    print("═" * 60)

    code = '(let ((x "hello")) (if (string? x) (string-length x) 0))'

    print(f"\n[OBSERVE] 输入代码:")
    print(f"  {code}")

    # Step 1: 类型检查（确认 string? 分支细化生效）
    print("\n[OBSERVE] aura --typecheck")
    tc = aura_typecheck(code)
    print(f"  → {tc}")

    # Step 2: 执行（string-length 走树遍历器）
    print("\n[VERIFY] aura (树遍历器)")
    eval_result = aura_eval(code)
    print(f"  → {eval_result}")

    ok = eval_result == "5"
    print(f"\n  {'✅' if ok else '❌'} 期望 5 (\"hello\" 的长度), 得到 {eval_result}")
    return ok


def demo_query_transform():
    """场景 3: AST 查询 + --ir 执行"""
    print("\n" + "═" * 60)
    print("📦 场景 3: AST 查询 + IR 执行 — 闭包求值")
    print("═" * 60)

    code = "(let ((x 10)) ((lambda (y) (+ x y)) 5))"

    print(f"\n[OBSERVE] 输入代码:")
    print(f"  {code}")

    # Step 1: 查询 Call 节点
    print("\n[OBSERVE] aura --query '(node-type Call)'")
    qr = aura_query(code, "(node-type Call)")
    print(f"  → {qr}")

    # Step 2: 查询 Lambda 节点
    print("\n[OBSERVE] aura --query '(node-type Lambda)'")
    qr = aura_query(code, "(node-type Lambda)")
    print(f"  → {qr}")

    # Step 3: IR 执行验证
    print("\n[VERIFY] aura --ir")
    ir = aura_ir(code)
    print(f"  → {ir}")

    ok = ir == "15"
    print(f"\n  {'✅' if ok else '❌'} 期望 15 (闭包 x=10, y=5), 得到 {ir}")
    return ok


def demo_incremental_serve():
    """场景 4: 增量编译 — serve 协议"""
    print("\n" + "═" * 60)
    print("📦 场景 4: 增量编译 — serve 协议 (define→exec→redefine)")
    print("═" * 60)

    # Persistent session: all commands to the same --serve process
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    def serve(cmd_type, code):
        payload = json.dumps({"cmd": cmd_type, "code": code})
        proc.stdin.write(payload + "\n")
        proc.stdin.flush()
        return proc.stdout.readline().strip()

    try:
        # Step 1: Define 函数
        print("\n[ACT] serve define: (define add (lambda (x y) (+ x y)))")
        result = serve("define", "(define add (lambda (x y) (+ x y)))")
        print(f"  → {result}")
        ok1 = "defined" in result

        # Step 2: Exec
        print("\n[ACT] serve exec: (add 1 2)")
        result = serve("exec", "(add 1 2)")
        print(f"  → {result}")
        ok2 = "value" in result and '"3"' in result

        # Step 3: Redefine (hot-swap)
        print("\n[ACT] serve redefine: (define add (lambda (x y) (+ (* x 2) y)))")
        result = serve("redefine", "(define add (lambda (x y) (+ (* x 2) y)))")
        print(f"  → {result}")
        ok3 = "redefined" in result

        # Step 4: Exec again
        print("\n[VERIFY] serve exec: (add 1 2) 热替换后")
        result = serve("exec", "(add 1 2)")
        print(f"  → {result}")
        ok4 = "value" in result and '"4"' in result

        ok = ok1 and ok2 and ok3 and ok4
        print(f"\n  {'✅' if ok else '❌'} 期望 add(1,2)=3, redef → add(1,2)=4")
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)
    return ok


def demo_closure_inspect_and_cache():
    """场景 5: 闭包内省 + 缓存圆环"""
    print("\n" + "═" * 60)
    print("📦 场景 5: 闭包内省 + 缓存圆环")
    print("═" * 60)

    code = "(let ((x 10)) ((lambda (y) (+ x y)) 5))"

    # Step 1: 闭包内省
    print(f"\n[OBSERVE] 输入代码:")
    print(f"  {code}")
    print("\n[OBSERVE] aura --inspect")
    r = subprocess.run(
        [AURA, "--inspect"], input=code, capture_output=True, text=True, timeout=10
    )
    output = r.stdout + r.stderr
    # Show first few lines
    for line in output.split("\n")[:8]:
        print(f"  {line}")

    # Step 2: 缓存
    tmp_cache = "/tmp/aura_closure_demo.abc"
    print(f"\n[ACT] aura --cache {tmp_cache}")
    cr = aura_cache(code, tmp_cache)
    print(f"  → cache written")

    # Step 3: 读缓存
    print("\n[OBSERVE] aura --cache-open")
    r = subprocess.run(
        [AURA, "--cache-open", tmp_cache], capture_output=True, text=True, timeout=10
    )
    cache_lines = r.stdout.strip().split("\n")
    for line in cache_lines[:4]:
        print(f"  {line}")

    # Step 4: IR 执行验证
    print("\n[VERIFY] aura --ir")
    ir = aura_ir(code)
    print(f"  → {ir}")

    ok = ir == "15"
    print(f"\n  {'✅' if ok else '❌'} 期望 15, 得到 {ir}")
    return ok


def demo_fibonacci():
    """场景 6: 递归 letrec 求值"""
    print("\n" + "═" * 60)
    print("📦 场景 6: 递归 — letrec + 尾递归 + 常量折叠")
    print("═" * 60)

    # 非尾递归阶乘
    code1 = (
        "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))"
    )
    print(f"\n[输入] 阶乘递归:")
    print(f"  {code1}")

    print("\n[VERIFY] aura --ir")
    ir1 = aura_ir(code1)
    print(f"  → {ir1}")

    # 斐波那契
    code2 = "(letrec ((fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))) (fib 10))"
    print(f"\n[输入] 斐波那契:")
    print(f"  {code2}")

    print("\n[VERIFY] aura --ir")
    ir2 = aura_ir(code2)
    print(f"  → {ir2}")

    # 简单加法（常量折叠）
    code3 = "(+ 1 2)"
    print(f"\n[输入] 常量折叠:")
    print(f"  {code3}")

    print("\n[VERIFY] aura --ir (PM: compute-kind→arity→const-fold)")
    ir3 = aura_ir(code3)
    print(f"  → {ir3}")

    ok1 = ir1 == "3628800"
    ok2 = ir2 == "55"
    ok3 = ir3 == "3"
    ok = ok1 and ok2 and ok3

    if not ok:
        print(f"\n  fact(10): {'✅' if ok1 else '❌'} (期望 3628800, 得到 {ir1})")
        print(f"  fib(10):  {'✅' if ok2 else '❌'} (期望 55, 得到 {ir2})")
        print(f"  (+ 1 2):  {'✅' if ok3 else '❌'} (期望 3, 得到 {ir3})")
    print(f"\n  {'✅' if ok else '❌'} 全部通过")
    return ok


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("╔" + "═" * 58 + "╗")
    print("║" + "  Aura AI Agent 端到端演示 ".center(56) + "║")
    print("║" + "  OBSERVE → DIAGNOSE → ACT → VERIFY ".center(56) + "║")
    print("╚" + "═" * 58 + "╝")

    if not os.path.exists(AURA):
        print(f"\n❌ 找不到 {AURA}")
        print(f"   先构建: cmake -B build && cmake --build build --target aura")
        sys.exit(1)

    tests = [
        ("类型错误修复", demo_type_error_fix),
        ("Occurrence Typing", demo_occurrence_typing),
        ("AST 查询 + IR", demo_query_transform),
        ("增量编译 serve", demo_incremental_serve),
        ("闭包内省+缓存", demo_closure_inspect_and_cache),
        ("递归 + 常量折叠", demo_fibonacci),
    ]

    passed = 0
    for name, fn in tests:
        try:
            ok = fn()
            if ok:
                passed += 1
                print(f"\n  ✅ {name}: PASS")
            else:
                print(f"\n  ❌ {name}: FAIL")
        except Exception as e:
            import traceback

            print(f"\n  ❌ {name}: ERROR — {e}")
            traceback.print_exc()
        print()

    print("═" * 60)
    print(f"结果: {passed}/{len(tests)} 通过")
    if passed == len(tests):
        print("✅ 全部通过 — Aura Agent 工具链工作正常")
    else:
        print("❌ 有失败场景")
        sys.exit(1)
