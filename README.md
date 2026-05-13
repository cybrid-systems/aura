# Aura

**为 AI Agent 设计的编程语言**  
从最小 Lisp 核心开始，自然生长。

---

## 当前状态（2026.05.12）

```
M0 种子     ✅  Racket #lang 原型
M1 C++求值  ✅  树遍历器 + ABF + IR 管线 (9/9)
M2 查询引擎 ✅  查询/变换/自动修复/--serve
M3a 语言补全  ✅  布尔/序对/begin/set!/quote/cond
M3b 宏系统   ✅  defmacro + 卫生宏 gensym + 编译期验证
M3c 反射     ✅  P2996 + IR opcode + Schema
M3d 类型系统 🔨  L6.1-L6.7 全线 ✅（L6.8+ 进行中）
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

### 🔨 M2 — AuraQuery 引擎（进行中）

| 特性 | 状态 |
|------|------|
| M2.1 ASTIndex (SoA 零拷贝过滤) | ✅ |
| M2.2 QueryEngine (S-表达式查询解析 + 执行) | ✅ |
| M2.3 TransformEngine (Patch 生成 + 应用) | ✅ |
| M2.4 --query / --query-and-fix CLI | ✅ |
| M2.5 SymRefIndex (符号引用倒排) | ✅ |
| M2.6 Hot swap (函数级 IR 替换) | ⬜ |
| M2.7 AutoFixEngine + --serve 模式 | ✅ |

**48 个 CTest 全部通过**

```bash
$ echo '(+ 1 2)' | ./aura --query '(node-type Call)'
query: 1 matches
$ echo '(+ x 1)' | ./aura --serve
S error kind=3 msg=unbound variable: x
S fix 4 patches
S fixed 0
$ ctest --test-dir build
100% tests passed, 0 tests failed out of 48
```

### 类型系统（进行中 — L6）

| 特性 | 状态 |
|------|------|
| L6.1 TypeRegistry (Int/Bool/String/Void/Any) | ✅ |
| L6.2 TypeAnnotationNode + ABF TAG-TYPE 0x0F | ✅ |
| L6.3 TypeChecker Pass 骨架 | ✅ |
| L6.4 Bi-dir 推断 + 约束求解 | ⬜ |
| L6.5 Query 类型 clause | ⬜ |
| L6.6 Gradual coercion | ⬜ |
| L6.7 Occurrence typing | ⬜ |

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
Agent (Racket/Python)
  │
  ├── /dev/stdin ──→ CompilerService ──→ EvalResult / Diagnostic
  │                    │
  │                    ├── Parser ──→ FlatAST (SoA, pmr arena)
  │                    ├── Tree-walker Evaluator (Phase 0)
  │                    └── IR Pipeline
  │                         ├── Lowering Pass (FlatAST → IR)
  │                         ├── PassManager (concept-based fold)
  │                         │   ├── ComputeKind
  │                         │   ├── ArityCheck
  │                         │   └── ConstantFolding
  │                         └── IRInterpreter (closure + cell heap)
  │
  └── --query / --query-and-fix / --serve / --auto-fix CLI
       └── QueryEngine → TransformEngine → apply_patches
```

### 源码结构

```
src/
├── core/          arena.ixx, ast.ixx, ast_flat.ixx, ast_pool.ixx, type.ixx, type_info.ixx
├── parser/        lexer.ixx, parser.ixx
├── compiler/
│   ├── frontend.ixx               — 树遍历求值器
│   ├── ir.ixx                     — AuraIR 指令集 (21 opcodes)
│   ├── lowering.ixx               — Expr/FlatAST → IR
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
ctest --test-dir build            # 48 个测试全部通过
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

## 设计哲学（核心原则）

1. **最小核心**：只保留最少的语义原语，所有上层语法都是宏（macro）。
2. **代码即数据**：程序与数据同构（homoiconicity），AI 可直接操作 AST。
3. **自然生长**：不拔苗助长，语言从真实语义需求中慢慢长大。
4. **窄门路线**：不兼容旧生态、不优化给人读、最终目标是统一并替换现有软件栈。

---

## 许可证

Apache 2.0
