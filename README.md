# Aura

**AI-native Lisp** — C++26 实现，IR 管线默认启用，**LLVM ORC JIT 后端** (fib-20: 7.55x vs tree-walker)。

**Status:** `build:ok` `test:100%` (`smoke 5/5 · unit 74/74 · integ 87/87 · typecheck 10/10 · bash 117/117`)

**Requirements:** GCC >= 16 (C++26 modules), CMake >= 4.0, Ninja >= 1.12

```bash
# CI (requires GCC 16+):
python3 build.py check
```

```bash
cmake -B build && cmake --build build --target aura -j

echo '(+ 1 2)' | ./build/aura                     # → 3
echo '(- 5 (* 2 3))' | ./build/aura --typecheck    # type: Int, result: -1
echo '(letrec ((fact ...)) (fact 10))' | ./build/aura --jit  # → 3628800 (LLVM JIT, 7.55x vs TW)
```

## 执行管线

```
输入 → 解析(lex+parse) → 宏展开 → fallback检测
  ├── 需求值器状态(EDSL/模块/特殊形式/副作用) → 树遍历求值器 (eval_flat)
  ├── 纯算术/原语/闭包/hash/pair → IR lowering → passes → IRInterpreter
  └── 使用 --jit flag        → IR lowering → passes → LLVM ORC JIT (native code)
```

## 基准 (fib-20)

| 执行模式 | 时间 | 加速比 |
|---------|------|-------|
| Tree-walker | 48.6ms | 1.00x |
| IR interpreter | 23.0ms | **2.11x** |
| LLVM JIT (-O2) | **6.4ms** | **7.55x** |

## 已实现

| 类别 | 内容 |
|------|------|
| **语言核心** | `apply`, variadic lambda, TCO, `let`/`let*`/`letrec`, `cond`, `when`/`unless` |
| **显式调用栈** | `std::variant` 驱动的外层 while 循环，支持 10 万级深递归 |
| **宏系统** | quasiquote, gensym, 递归展开, dotted rest param, **卫生宏 (name_map自动重命名)** |
| **模块** | `require`/`import` 前缀注入, `export` 控制, 循环检测, 自动 lib 发现 |
| **类型系统** | **Sound Gradual Typing**: coercion + CastOp, `forall` let-poly, occurrence typing, type query, blame labels, `(cast ...)`, `(check ...)`, `(: ...)` 标注语法 |
| **类型 IR 集成** | `IRInstruction.type_id`, 运行时类型断言(strict), Let-Poly, TypeSpecializationPass |
| **错误处理** | `try`/`catch`/`raise`/`assert`, 原语返回 error 不崩溃, **编译期 AST 验证** |
| **数据结构** | pair/list, vector, hash table, variadic functions |
| **I/O** | `display`, `write`, `read-file`, `write-file`, `file-copy`, `file-delete` |
| **标准库** | `hash`, `combinators`, `maybe`, `csv`, `set`, `io`, `list`, `math`, `string`, `test`, `iter`, `queue`, `stack`, `random`, **`datetime`**, **`json`**, **`validate`**, **`struct`** (19 lib) |
| **CaaS 服务** | `--serve` with `compile`/`eval`/`module`/`define`/`config` 命令 |
| **增量编译** | ArenaGroup 多模块, `reload_module` dirty-only, mmap 磁盘缓存, 函数热替换 |
| **IR 管线** | 38 opcode, const folding, compute-kind, arity check, 闭包桥接, 类型特化 pass, **IR 级 import** |
| **LLVM JIT** | **ORC JIT 后端**, 38 opcode native code, LLVM -O2 优化, 增量 JIT 缓存, 闭包/Cell/Pair/PrimCall bridge, `--jit` flag (fib-20 **7.55x** vs TW) |
| **EDSL / AI Agent** | `set-code`, `query:*`, `mutate:*`, `typecheck-current`, `eval-current`, `current-source`, LLM auto-test pipeline |

## 项目结构

```
src/core/         FlatAST(SoA), StringPool, arena, type system     ~2.7k
src/parser/       lexer + s-expr → FlatAST                         ~1.4k
src/compiler/     IR(lowering+passes+interpreter), 树遍历求值器,
                  类型检查(typeck + let-poly + specialization),
                  CaaS 服务, EDSL                                    ~10k
lib/std/          hash, combinator, maybe, csv, set, io, list, map,
                  for-each, math, string, test, iter, queue,
                  stack, random, json, struct, validate             ~1k lines
tests/            bash(117), C++ unit(74/74), integ(87/87), smoke(5/5),
                  benchmark(44), mutation, AI agent demo              ~6k
```

## 快速体验

```bash
# hash 表
# current-time + datetime
printf '(require std/datetime)(timestamp->iso-date (timestamp))\n' | ./build/aura

# hash 表
printf '(require std/hash)(define h (hash "a" 1))(hash-set h "b" 2)\n(hash->list h)\n' | ./build/aura

# variadic lambda
echo '((lambda xs (length xs)) 1 2 3)' | ./build/aura           # → 3

# apply
echo '(apply + (list 1 2 3 4 5))' | ./build/aura                # → 15

# 组合器
printf '(require std/combinators)((compose (lambda (x) (+ x 1))\n(lambda (x) (* x 2))) 5)\n' | ./build/aura

# CSV 解析
printf '(require std/csv)(csv-parse "a,b,c\\n1,2,3")\n' | ./build/aura

# 严格类型检查
echo '(- 5 (* 2 3))' | ./build/aura --strict                     # type error on mixed type
./build/aura --serve <<< 'config strict true\neval (+ "1" 2)'   # type error

# CaaS serve
printf '(module compile "demo" "(define (f x) (+ x 1))")(module eval demo "(f 41)")\n' | ./build/aura --serve

# 管道多表达式
printf '(define h (hash "a" 1))\n(hash-set! h "b" 2)\n(hash-length h)\n' | ./build/aura
```

## 极限：当前能做到什么规模

Aura 当前适用于：
- 单文件 500 行以内的脚本
- 哈希表/CSV 数据处理
- 函数式组合（compose/curry/filter/map/foldl）
- EDSL 驱动的 AI agent 代码变换（`set-code` → `mutate:rebind` → `current-source` → `eval-current`）
- 通过 `--serve` 做 CaaS 增量服务
- 类型安全的表达式求值（`--strict` 模式）

不适合：
- 大型面向对象系统（无 class、无 GC）
- 需要高吞吐 JSON/网络 IO（无 socket 原语）

## LLM Agent 开发（EDSL 管线）

Aura 内建 EDSL 管线支持 LLM 驱动的代码生成 → 执行 → AST 变换修复循环。

```bash
# 安装依赖：DeepSeek v4 Flash
pip install requests

# 运行 AI agent（需要 LLM_API_KEY）
LLM_API_KEY="sk-..." LLM_MODEL="deepseek-v4-flash" \
  python3 tests/ai_agent_iter.py "Write (fact n) that computes factorial"

# EDSL 精确修复（set-code + query + mutate + current-source）
LLM_API_KEY="sk-..." LLM_MODEL="deepseek-v4-flash" \
  python3 tests/ai_agent_edsl.py "Write (range start end) that returns list of ints"
```

### EDSL AST→Source 桥接

```lisp
;; 1. 锁定破损代码到工作区
(set-code "(define (bad-fact n) (if (= n 0) 1 (* n (bad-fac (- n 1)))))")

;; 2. 定位函数节点
(query:find "bad-fact")  ;; → (16)

;; 3. 用 mutate:rebind 修复
(mutate:rebind "bad-fact"
  "(define (bad-fact n) (if (= n 0) 1 (* n (bad-fact (- n 1)))))"
  "fix typo")

;; 4. AST→Source 给 LLM 看更新后的代码
(current-source)
;; → "(define bad-fact (lambda (n) (if (= n 0) 1 (* n (bad-fact (- n 1))))))"

;; 5. 验证
(eval-current)
(bad-fact 5)  ;; → 120
```

### LLM 自动测试报告（5/19）

| 任务 | 结果 |
|------|------|
| factorial, fibonacci, map, prime?, **datetime** | ✅ 一次性 |
| merge-sort, flatten-tree, palindrome? | ✅ 一次性 |
| `set!` closure counter, try/catch, hash operations | ✅ 已验证 |
| EDSL typo fix (bad-fac → bad-fact) | ✅ 完整管线 |
| pluck/where with hashes + stdlib | ✅ |

## 测试

```bash
python3 build.py test smoke    # 冒烟 5/5
python3 build.py test unit     # C++ 单元 74/74
python3 build.py test integ    # 端到端 87/87
python3 build.py test bash     # 回归 117/117
python3 build.py test bench    # 基准 44/44
python3 build.py test          # 全部
```

## 文档

- **[docs/tutorial.md](docs/tutorial.md)** — 10 分钟快速入门
- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/known_issues.md](docs/known_issues.md) — 已知问题

## License

Apache 2.0
