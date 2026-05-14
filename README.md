# Aura

**为 AI Agent 设计的编程语言**  
从最小 Lisp 核心开始，自然生长。

---

## 当前状态（2026.05.14）

```
M0 种子     ✅  Racket #lang 原型
M1 C++求值  ✅  EvalValue + 树遍历器 + ABF + IR 管线
M2 查询引擎 ✅  查询/变换/自动修复/--serve
M3a 语言补全  ✅  布尔/序对/begin/set!/quote/cond
M3b 宏系统   ✅  defmacro + 卫生宏 gensym + 编译期验证
M3c 反射     ✅  P2996 + IR opcode + Schema
M3d 类型系统 ✅  L6.1-L6.7 全线完成
M3e 工具链   ✅  Benchmark + IR 递归修复 + EvalValue variant
M3f 增量编译 ✅  定义缓存 → 依赖追踪 → 增量 Pass → --serve 协议
M3g AI 闭环   ✅  mutation_loop.py + LLM --ai 驱动变异
M4 生产     ⬜  LLVM + AOT + 自举
```

### ✅ M1 — C++ 求值器（完成）

| 层 | 特性 | 状态 |
|---|------|------|
| 树遍历求值器 | L1.1-L1.8: 整数/变量/算术/if/闭包/letrec/define/REPL | ✅ |
| ABF 反序列化 | Racket 输出 → C++ Expr 结构等价 | ✅ |
| AuraIR 管线 | 21 opcodes, Lowering Pass, IR 解释器 | ✅ |
| 内存池 | pmr SmallObjectPool 三级 bump + ArenaGroup | ✅ |
| 闭包变换 | 自由变量捕获, MakeClosure + Capture | ✅ |
| compute-kind | Known/Unknown 数据流分析 | ✅ |
| Arity 检查 | 编译期参数数量校验 | ✅ |
| 常量折叠 | 编译期算术折叠 | ✅ |
| letrec IR | mutable cell heap 支持递归闭包 | ✅ |
| Pass Manager | concept-based fold pipeline | ✅ |
| C++ 现代化 | Parser/Lowering 纯函数化, EvalResult → std::expected | ✅ |
| EvalValue variant | 消除 sentinel 魔法数字, variant<int64_t, bool, StringRef, ClosureRef, CellRef, PairRef> | ✅ |

### 🔨 M2 — AuraQuery 引擎（进行中）

| 特性 | 状态 |
|------|------|
| M2.1 ASTIndex (SoA 零拷贝过滤) | ✅ |
| M2.2 QueryEngine (S-表达式查询解析 + 执行) | ✅ |
| M2.3 TransformEngine (Patch 生成 + 应用) | ✅ |
| M2.4 --query / --query-and-fix CLI | ✅ |
| M2.5 SymRefIndex (符号引用倒排) | ✅ |
| M2.6 Hot swap (函数级 IR 替换) | ✅ |
| M2.7 AutoFixEngine + --serve 模式 | ✅ |

**61 个 CTest + 42 个 Benchmark + 33 个 Integration + 5 个 Smoke = 147 测试全部通过**

```bash
$ echo '(+ 1 2)' | ./aura --query '(node-type Call)'
query: 1 matches
$ python3 tests/benchmark.py
42/42 PASS — 0.14s
$ python3 build.py test
All 5 test suites passed
```

### ✅ M3f — 增量编译 + Compiler as a Service（完成）

| 特性 | 状态 |
|------|------|
| Phase 1: 定义分离 — try_extract_define 检测 DefineNode | ✅ |
| Phase 2: IR 缓存 — cache-aware lowering, 函数自动内联 | ✅ |
| Phase 3: 依赖追踪 — BFS 传递闭包失效 + 从源码重降低 | ✅ |
| Phase 4: 增量 Pass — per-function compute-kind + const-fold | ✅ |
| Phase 5: --serve 协议 — JSON define/exec/redefine 命令 | ✅ |
| Hot swap (M2.6) | ✅ |
| 跨行缓存保持 — 同一 CompilerService 实例跨输入串行执行 | ✅ |
| 先使用后定义 — 优雅降级 (未缓存时全量降低) | ✅ |

### 🦾 Agent 变异循环（完成）

```bash
$ python3 tests/mutation_loop.py --demo                     # 展示 11 种变异策略
$ python3 tests/mutation_loop.py tests/fixtures/basic_add.aura  # 一次变异测试
$ python3 tests/mutation_loop.py --loop tests/fixtures/fib_10.aura --fast  # 10 代持续变异
$ LLM_API_KEY=... python3 tests/mutation_loop.py --ai tests/fixtures/fib_10.aura  # LLM 驱动变异
```

11 种变异策略覆盖：整数常量变更、操作符交换、语义保留包装、if 分支交换、let 绑定引入。
LLM 驱动：通过 OpenAI-compatible API (DeepSeek/OpenAI) 用 AI 生成代码变异。
闭环管线：变异 → 编译 → 执行验证 → benchmark 回归检测 → 保留/丢弃。

### 类型系统（完成 ✅ — L6）

| 特性 | 状态 |
|------|------|
| L6.1 TypeRegistry (Int/Bool/String/Void/Any) | ✅ |
| L6.2 TypeAnnotationNode + ABF TAG-TYPE 0x0F | ✅ |
| L6.3 TypeChecker Pass 骨架 | ✅ |
| L6.4 Bi-dir 推断 + 约束求解 | ✅ |
| L6.5 Query 类型 clause — (type? x "Int"), type-of 返回 Type | ✅ |
| L6.6 Gradual coercion | ✅ |
| L6.7 Occurrence typing — string?/number?/type? + not/and/or | ✅ |

Full design: [`docs/aura_typesystem.md`](docs/aura_typesystem.md)
Formal rules: [`docs/aura_typesystem_formal.md`](docs/aura_typesystem_formal.md)

### Racket Agent Demo

```bash
$ racket tests/agent_demo.rkt
# submit (+ x 1) → detect error → query AST → fix → verify 43 ✓
```

---

## 架构概览

```
Agent (Racket/Python / mutation_loop.py)
  │
  ├── /dev/stdin ──→ CompilerService (persistent session)
  │                    │
  │                    ├── Parser ──→ FlatAST (SoA, pmr arena) ──→ Flat TypeChecker
  │                    ├── Tree-walker Evaluator (env persistence)
  │                    ├── IR Pipeline
  │                    │    ├── Lowering (FlatAST → IR, cache-aware)
  │                    │    ├── IR Cache (by-name function storage)
  │                    │    ├── PassManager (compute-kind → arity → const-fold)
  │                    │    └── IRInterpreter (closures + cells + coercion)
  │                    └── Hot Swap Engine (runtime function replacement)
  │
  └── CLI
       ├── --query / --query-and-fix (AST query + transform)
       ├── --serve (JSON protocol: define/exec/redefine)
       ├── --hot-swap (runtime function replacement)
       ├── --typecheck (positions: line:col)
       └── mutation_loop.py (AI-driven evolution loop)
```

### 源码结构

```
src/
├── core/          arena.ixx, ast.ixx, ast_flat.ixx, ast_pool.ixx, type.ixx, type_info.ixx
├── parser/        lexer.ixx, parser.ixx
├── compiler/
│   ├── frontend.ixx               — 树遍历求值器
│   ├── ir.ixx                     — AuraIR 指令集 (21 opcodes)
│   ├── lowering.ixx               — FlatAST → IR (cache-aware)
│   ├── ir_interpreter.ixx         — IR 解释器
│   ├── pass_manager.ixx           — concept-based Pass pipeline
│   ├── compute_kind.ixx           — Known/Unknown 分析
│   ├── arity.ixx                  — 参数数量校验
│   ├── diag.ixx                   — 结构化错误诊断
│   ├── service.ixx                — CompilerService
│   ├── query.ixx                  — ASTIndex/QueryEngine/TransformEngine/SymRefIndex/AutoFix
│   └── type_checker.ixx           — TypeChecker 骨架 (L6.3+)
├── binary/        abf_deserializer.ixx
└── main.cpp                       — CLI: REPL / pipe / --ir / --query / --serve / ...
tests/
├── test_ir.cpp                    — IR 管线/查询/内存池集成测试
└── agent_demo.rkt                 — Racket Agent 自动修复演示
```

---

## 快速开始

### 构建

```bash
cmake -B build
cmake --build build --target aura
```

### 运行

```bash
# 树遍历模式 (默认)
echo '(+ 1 2)' | ./aura          # → 3

# IR 管线模式
echo '((lambda (x) (* x 2)) 5)' | ./aura --ir  # → 10

# 查询 AST
echo '(+ 1 2)' | ./aura --query '(node-type LiteralInt)'  # → 2 matches

# 变换 AST
echo '(+ 1 2)' | ./aura --query-and-fix '(node-type LiteralInt)' '(LiteralInt 99)'

# 自动修复
echo '(+ x 1)' | ./aura --serve   # → error + auto-fix

# REPL 模式
./aura
```

### 测试

```bash
./build/test_ir                   # 61 个 C++ 单元测试
python3 tests/benchmark.py        # 42 个 benchmark 基线
python3 build.py test all         # 全部 5 套件, 147 条测试
python3 tests/mutation_loop.py --demo  # 展示变异策略
python3 tests/mutation_loop.py --loop tests/fixtures/fib_10.aura --fast  # 10 代变异循环
racket tests/agent_demo.rkt       # Agent 自动修复演示
```

---

## 设计仓库

> 完整设计文档见 [**ai-programming-language-design**](https://github.com/cybrid-systems/ai-programming-language-design) — 架构设计、序列化协议、AuraQuery eDSL、C++26 模块方案。

---

## 为什么叫 Aura？

Aura（光环 / 灵气）代表语言在语义空间中散发出的**无形却无所不在的能量场**。  
AI Agent 在这个光环里自由地操作代码，最终让整个软件栈自然演化成新的形态。

---

## 下一步计划

| 项目 | 优先级 | 描述 |
|------|--------|------|
| 项目 | 优先级 | 描述 |
|------|--------|------|
| Agent 变异循环深化 | 🔜 当前 | 跨文件演化, 多策略组合, 接入更多 LLM |
| 持续基准回归 | ⬜ 短期 | CI 集成 benchmark --check |
| L6.8+ 多态类型 | ⬜ 中期 | ∀a. T 多态 + 类型类 |
| M4 LLVM + AOT | ⬜ 长期 | LLVM 后端 + 原生编译 |

## 设计哲学（核心原则）

1. **最小核心**：只保留最少的语义原语，所有上层语法都是宏（macro）。
2. **代码即数据**：程序与数据同构（homoiconicity），AI 可直接操作 AST。
3. **自然生长**：不拔苗助长，语言从真实语义需求中慢慢长大。
4. **窄门路线**：不兼容旧生态、不优化给人读、最终目标是统一并替换现有软件栈。

---

## 许可证

Apache 2.0
