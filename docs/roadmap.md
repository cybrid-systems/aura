# Aura — 实现进度跟踪

---

## 里程碑状态

```
M0 种子     ✅  Racket #lang 原型
M1 C++求值  ✅  树遍历器 + ABF + IR 管线 (9/9)
M2 查询引擎 🔨  AST查询/变换/自动修复 (6/7)
M3 反射     ⬜
M4 生产     ⬜
```

---

## 里程碑 M1 — C++ 求值器 ✅

### 🏗 架构

| Step | 状态 |
|------|------|
| CMake 4.0 + C++26 模块骨架 | ✅ |
| CLI 文本模式 + REPL | ✅ |
| ABF 序列化 (Racket端) | ✅ |
| ABF v2 反序列化 (C++端) | ✅ |
| pmr 内存池 (v3: SmallObjectPool) | ✅ |
| CompilerService | ✅ |

### 🗣 语言 — Tree-Walker

| Step | 状态 |
|------|------|
| L1.1-L1.8: 整数/变量/算术/if/闭包/letrec/define/REPL | ✅ 全部 |

### 🗣 IR 管线

| Step | 状态 |
|------|------|
| A2.1 AuraIR 指令集 (21 opcodes) | ✅ |
| A2.2 Lowering Pass (Expr/FlatAST → IR) | ✅ |
| A2.3 Pass Manager (concept-based fold) | ✅ |
| L2.1 IR 解释器 | ✅ |
| L2.2 闭包变换 | ✅ |
| L2.3 compute-kind 分析 | ✅ |
| L2.4 Arity 检查 | ✅ |
| L2.5 常量折叠 | ✅ |
| L2.6 letrec IR (mutable cell) | ✅ |

### 🔧 基建

| Step | 状态 |
|------|------|
| CTest (29 tests) | ✅ |
| test_ir 集成测试 | ✅ |

### 🏗 C++ 现代化重构

| Step | 状态 |
|------|------|
| Pass concept + fold pipeline | ✅ |
| Parser → 纯函数 parse() | ✅ |
| LoweringPass → 纯函数 lower_to_ir() | ✅ |
| EvalResult → std::expected | ✅ |
| FlatAST SoA + StringPool | ✅ |

---

## 里程碑 M2 — AuraQuery 引擎 🔨

| Step | 特性 | 红线 | 状态 |
|------|------|------|------|
| M2.1 | ASTIndex — SoA 零拷贝过滤 | `by_tag(Call)` on (+ 1 2) → 1 | ✅ |
| M2.2 | QueryEngine — S-表达式查询 | `(callee "+")` → 1 match | ✅ |
| M2.3 | TransformEngine — Patch 生成 | `query-and-fix` → applied=true | ✅ |
| M2.4 | CLI: --query / --query-and-fix | CLI 模式可用 | ✅ |
| M2.5 | SymRefIndex — 符号引用倒排 | `count("x")` → refs | ✅ |
| M2.6 | Hot swap — 函数级 IR 替换 | 运行时替换 | ⬜ |
| M2.7 | AutoFixEngine + --serve 模式 | 编译→报错→自动修复→验证 | ✅ |

### 端到端 Demo

```bash
$ racket tests/agent_demo.rkt
# submit (+ x 1) → detect error → query AST → fix → verify 43 ✓
```

---

## 设计文档

完整设计文档见 [ai-programming-language-design](https://github.com/cybrid-systems/ai-programming-language-design):
- `docs/aura_query_engine.md` — M2 查询引擎设计
- `docs/aura_ast_dod.md` — 扁平 AST SoA 设计
- `docs/aura_cpp26_guide.md` — C++ 现代化指南
- `docs/aura_memory_pool.md` — 内存池设计
- `docs/aura_caas.md` — CompilerService 集成
