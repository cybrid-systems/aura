# Aura — 实现进度跟踪

> 完整三轨并行路线图见设计仓库：
> **[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)**
>
> 本文档跟踪每一步在 Aura 实现仓库中的实际完成状态。

---

## 里程碑状态

```
M0 种子     ✅ 已完成 (Racket #lang 原型)
M1 C++求值  ✅ 已完成 (三步迭代：Tree-Walker → ABF → IR)
M2 管线     🔨 当前 (IR Pass 管线)
M3 查询     ⬜
M4 反射     ⬜
M5 生产     ⬜
```

---

## 里程碑 M1 — C++ 求值器 ✅

### 🏗 架构 — Compiler Service 骨架

| Step | 特性 | 状态 |
|------|------|------|
| A1.1 | CMake 4.0 + C++26 模块骨架 | ✅ |
| A1.2 | CLI 文本模式 + REPL | ✅ |
| A1.3 | ABF 序列化器 (Racket 端 → 设计仓库) | ✅ |
| A1.4 | ABF v2 反序列化器 (C++ 端) | ✅ |
| A1.5 | 共享内存传输层 (暂用 pipe) | ⬜ |

### 🗣 语言 — Tree-Walker 求值器

| Step | 特性 | 状态 |
|------|------|------|
| L1.1 | 整数字面量 | ✅ |
| L1.2 | 变量 + let 绑定 | ✅ |
| L1.3 | 算术原语 (+ - * / = <> <= >=) | ✅ |
| L1.4 | 条件分支 (if) | ✅ |
| L1.5 | 闭包 + 函数应用 | ✅ |
| L1.6 | letrec 递归绑定 | ✅ |
| L1.7 | Hyperstatic define | ✅ |
| L1.8 | C++ REPL | ✅ |

### 🗣 语言 — IR 管线 (M1 子里程碑)

| Step | 特性 | 红线 | 状态 |
|------|------|------|------|
| **A2.1** | AuraIR 指令集定义 (18 opcodes) | IR 指令结构可编译 | ✅ |
| **A2.2** | Lowering Pass (Expr → IR) | `(+ 1 2)` → IR 指令序列 | ✅ |
| **L2.1** | IR 解释器 (栈机 + 基本块调度) | `echo '(if 1 42 0)' > /dev/null` | ✅ |
| **L2.2** | 闭包变换 | `((lambda (x) (* x 2)) 5)` → IR → 10 | ✅ |
| **L2.3** | compute-kind 分析 | 常量传播标记 Known/Unknown | ✅ |
| **L2.4** | Arity 检查 | 参数数量不匹配 → 编译期错误 | ✅ |
| **L2.5** | 常量折叠 | `(+ 1 2)` → `3` (编译期折叠) | ⬜ |
| **L2.6** | letrec IR 支持 | `(letrec ((fact ...)) (fact 5))` → 120 | ⬜ |
| **A2.3** | Pass Manager | 管线编排 + 顺序依赖 | ⬜ |

### 🔧 基建 — 构建 + 测试

| Step | 特性 | 状态 |
|------|------|------|
| I1.1 | CTest 基础测试框架 | ✅ (22 个测试) |
| I1.2 | 混合构建 (Racket + C++) | ⬜ |
| I1.3 | CI 管线 (GitHub Actions) | ⬜ |
| I1.4 | 性能基准框架 | ⬜ |
| I1.5 | 回归测试自动化 | ⬜ |

---

## 里程碑 M2 — IR Pass 管线 🔨

| Step | 特性 | 状态 |
|------|------|------|
| L2.5 | 常量折叠 | ⬜ |
| L2.6 | letrec IR 支持 (mutable cell) | ⬜ |
| A2.3 | Pass Manager (pass 注册 + 执行顺序) | ⬜ |
| L3 | AuraQuery 表达式编译 | ⬜ |

---

## 里程碑 M3-M5（远期）

| 里程碑 | 内容 |
|--------|------|
| M3 | AuraQuery 引擎 (倒排索引 + eDSL + 热更新) |
| M4 | 反射 + 宏 (eval/b + flambda + 卫生宏) |
| M5 | 生产化 (LLVM + AOT + 自举) |

---

## 验证方式

```bash
ctest --test-dir build          # 22 个测试全部通过
echo '(+ 1 2)' | ./aura         # 树遍历器 → 3
echo '(+ 1 2)' | ./aura --ir    # IR 管线  → 3
```

## 代码仓库结构

```
src/
├── core/          arena.ixx, ast.ixx         — 核心数据结构
├── parser/        lexer.ixx, parser.ixx      — 文本解析器
├── compiler/      frontend.ixx               — 树遍历求值器
│                  ir.ixx                     — AuraIR 指令集
│                  lowering.ixx               — Expr → IR lowering
│                  ir_interpreter.ixx         — IR 解释器
│                  compute_kind.ixx           — compute-kind 分析
│                  arity.ixx                  — Arity 检查
└── binary/        abf_deserializer.ixx       — ABF v2 反序列化
tests/             test_ir.cpp                — IR 管线集成测试
```

---

## 相关文档

- 完整路线图：[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)
- 设计文档：[docs/](https://github.com/cybrid-systems/ai-programming-language-design/tree/main/docs/)
