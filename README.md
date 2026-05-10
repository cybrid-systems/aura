# Aura

**为 AI Agent 设计的编程语言**  
从最小 Lisp 核心开始，自然生长。

---

## 当前状态（2026.05.11）

```
M0 种子     ✅  Racket #lang 原型
M1 C++求值  ✅  树遍历器 + ABF + IR 管线
M2 管线     🔨  IR Pass 管线（当前工作）
M3 查询     ⬜  AuraQuery 引擎
M4 反射     ⬜
M5 生产     ⬜  LLVM + AOT + 自举
```

### ✅ M1 完成交付

| 层 | 特性 | 验证 |
|---|------|------|
| 树遍历求值器 | L1.1-L1.8: 整数/变量/算术/if/闭包/letrec/define/REPL | 13 个 CTest |
| ABF 反序列化 | Racket 输出 → C++ Expr 结构等价 | pipe 模式通过 |
| AuraIR 管线 | 18 opcodes, Lowering Pass, IR 解释器 | 9 个 IR CTest |
| 闭包变换 | 自由变量捕获, MakeClosure + Capture 运行时 | `((lambda (y) (+ x y)) 5)` → 15 |
| compute-kind | Known/Unknown 数据流分析 | 常量传播正确标记 |
| Arity 检查 | 编译期参数数量校验 | 参数不匹配 → 编译期错误 |

**总计：22 个 CTest 全部通过**

```bash
$ echo '((lambda (x) (* x 2)) 5)' | ./aura --ir
10
$ echo '((lambda (x y) (+ x y)) 3 4)' | ./aura
7
$ ctest --test-dir build
100% tests passed, 0 tests failed out of 22
```

### 🔨 M2 当前工作

- [ ] L2.5: 常量折叠 — `(+ 1 2)` → `3` 编译期折叠
- [ ] L2.6: letrec IR 支持 — mutable cell 闭环
- [ ] A2.3: Pass Manager — pass 注册 + 执行顺序

---

## 架构概览

```
输入 (文本)                 IR 管线                   输出
┌────────┐   parse()   ┌──────────┐   analyze/opt   ┌──────────┐
│ S-Expr  │ ────────→  │ AuraIR   │ ─────────────→  │ 结果     │
│ 文本    │            │ IRModule │                 │ int64    │
└────────┘            └──────────┘                 └──────────┘
                           │
                     ┌─────┴──────┐
                     │ Pass 管线  │
                     │ (进行中)   │
                     └────────────┘
```

### 源码结构

```
src/
├── core/          arena.ixx, ast.ixx         — 核心数据结构
├── parser/        lexer.ixx, parser.ixx      — 文本 S-Expr 解析器
├── compiler/
│   ├── frontend.ixx               — 树遍历求值器 (Phase 0)
│   ├── ir.ixx                     — AuraIR 指令集 (18 opcodes)
│   ├── lowering.ixx               — Expr → IR lowering
│   ├── ir_interpreter.ixx         — IR 解释器
│   ├── compute_kind.ixx           — compute-kind 分析
│   └── arity.ixx                  — Arity 检查
├── binary/
│   └── abf_deserializer.ixx       — ABF v2 反序列化
└── main.cpp                       — CLI: REPL / pipe / --ir / --abf
tests/
└── test_ir.cpp                    — IR 管线集成测试
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
echo '(let ((x 10)) x)' | ./aura  # → 10

# REPL 模式
./aura                            # 交互式输入

# IR 管线模式
echo '((lambda (x) (* x 2)) 5)' | ./aura --ir  # → 10
```

### 测试

```bash
ctest --test-dir build            # 22 个测试全部通过
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
