# Aura

**AI-native Lisp** — C++26, IR 管线, **LLVM ORC JIT**, Sound Gradual Typing.

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2 3)' | ./build/aura                  # → 6 (JIT auto)
printf '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))' | ./build/aura --jit  # → 3628800 (LLVM JIT)
echo '(- 5 (* 2 3))' | ./build/aura --typecheck  # type: Int, result: -1
```

**Status:** `build:ok` `test:100%` (smoke 5/5 · unit 74/74 · integ 87/87 · typecheck 10/10 · bash 117/117)

**Requirements:** GCC >= 16 (C++26 modules), CMake >= 4.0, Ninja >= 1.12

## 基准

| 模式 | fib-20 | 加速比 |
|------|--------|-------|
| Tree-walker | 48.6ms | 1.0x |
| IR interpreter | 23.0ms | 2.1x |
| **LLVM JIT (-O2)** | **6.4ms** | **7.55x** |

JIT 自动集成：算术表达式透明走 JIT，复合类型/string/bool 透明回落 eval。

## 执行管线

```
输入 → 解析 → 宏展开 → AST验证 → 类型检查
  → define? → 缓存 + eval-flat
  → IR lowering → 【JIT 编译 (算术) / IR解释 (其他)】 → EvalValue
  → EDSL(set-code/query/mutate) → 独立 AST 变换 → 不影响求值
```

## 特性

### 语言核心
`apply`, variadic lambda, TCO, let/let\*/letrec, cond, when/unless, set!, quasiquote, **卫生宏**, `require`/`import` 模块系统

### 类型系统 — Sound Gradual Typing 🟢 10/10
- Coercion + CastOp 运行时类型检测
- `forall` let-polymorphism
- **Occurrence typing**: `(if (string? x) (append x "!") x)` 分支类型细化
- Type annotation: `(: x Int)`, `(cast expr : Int)`, `(check expr : Int)`
- Blame labels: `TypeError at 1:2: expected Int, got String`
- `type-of`, `type?` 运行时类型反射
- `--strict` 严格模式 + 增量缓存

### LLVM JIT 🟢 10/10
- ORC JIT, 38 opcode → native code
- LLVM -O2 优化管线 (inline, GVN, DCE, LICM)
- 增量 JIT 缓存 + 依赖失效
- 闭包/Cell/Pair/PrimCall bridge
- 7.55x vs tree-walker (fib-20)

### 数据结构 & I/O
pair/list, vector, hash table, `display`/`write`, `read-file`/`write-file`, `try`/`catch`/`raise`

### 标准库 (19 libs, ~1k lines)
hash, combinators, maybe, csv, set, io, list, math, string, test, iter, queue, stack, random, datetime, json, struct, validate

### EDSL / AI Agent
```lisp
(set-code "(define (f n) (bad-fac n))")
(query:find "bad-fac")            ; → (16)
(mutate:rebind "bad-fac" "...")   ; 修正
(current-source)                   ; → 源码
(eval-current)                     ; → 验证
```
LLM 驱动闭循环 (iter + EDSL), 支持 DeepSeek v4 Flash

### C FFI 🟢
`c-load`/`c-func`: dlopen 动态库, Int/Float/String/Opaque marshalling, JIT 符号注册

### CaaS 服务
`--serve` JSON 协议: compile / eval / module / define / config / ml 热替换

## 项目规模

```
src/core/      FlatAST(SoA), 类型系统              ~2.7k
src/parser/    lexer + parser                     ~1.4k
src/compiler/  IR管线+求值器+类型检查+EDSL+JIT      ~10k
lib/std/       19 个库                             ~1k
tests/         bash(117)+unit+integ+bench+agent    ~6k
```

## 基准

- [docs/benchmark.md](docs/benchmark.md) — EDSL 模型能力基准 (deepseek-v4-flash: 92% stable)
- [tests/edsl_benchmark.py](tests/edsl_benchmark.py) — 运行：`LLM_API_KEY="..." python3 tests/edsl_benchmark.py --rounds 3 --fix --max-attempts 5`

## 文档

- [docs/tutorial.md](docs/tutorial.md) — 10 分钟入门
- [docs/roadmap.md](docs/roadmap.md)
- [docs/known_issues.md](docs/known_issues.md)
- [design repo](https://github.com/cybrid-systems/ai-programming-language-design) — architecture / type system / reflection

## License

Apache 2.0
