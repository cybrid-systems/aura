# Aura — 路线图

**更新：2026-05-19** — D1 LLVM JIT + D2 Sound Gradual Typing 完成。106/106 测试全绿。

---

## 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 10/10 | TW + IR 双路径 + 显式调用栈 |
| **类型系统** | 🟢 10/10 | Sound Gradual + coercion + occurrence + let-poly + type query + blame |
| LLVM JIT | 🟢 10/10 | ORC JIT, 38 opcode native, -O2, 增量 cache, 闭包/Pair/PrimCall |
| 编译器基础设施 | 🟢 9/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / IR import |
| 测试覆盖 | 🟢 8/10 | integ 87 + unit 74 + smoke 5 + bash 117 + bench 44 |
| 标准库 | 🟢 8/10 | 19 文件 ~1k 行 |
| 错误处理 | 🟢 9/10 | try/catch IR + diag + AST validate |
| EDSL / AI Agent | 🟢 10/10 | set-code/query/mutate/typecheck + LLM pipeline + iter correction |
| 文档 | 🟢 9/10 | README + tutorial + design repo + intent orchestration design |

---

## 已完成

### Phase A-D: 核心功能
| Phase | 主要内容 | 状态 |
|-------|---------|------|
| A | Tree-walker, 宏, 模块, eval-var, closure bridge | ✅ |
| B | 增量编译, 类型系统 L6, diagnostics, CI/CD | ✅ |
| C | 卫生宏, AST 验证, IR import, stdlib v3 | ✅ |
| **D1** | **LLVM JIT** — ORC 编译, 算术/闭包/Cell/Pair/CastOp, PrimCall bridge, -O2, 增量 cache | ✅ |
| **D2** | **Sound Gradual Typing** — Coercion, CastOp, bi-directional check, occurrence, type-of, blame, type query | ✅ |

### Phase D1: LLVM ORC JIT
```
fib-20: TW 48.6ms → IR 23.0ms → JIT 6.4ms (7.55x)
```
- P1 基础架构: AuraJIT, ORC LLJIT ✅
- P2 算术: 38 opcode, 控制流, 比较 ✅
- P3 闭包+Cell: 捕获修复, 递归闭包 ✅
- P4 运行时: PrimCall bridge, display, eval 集成 ✅
- P5 优化: LLVM -O2 PassBuilder, 增量 cache ✅

### Phase D2: Sound Gradual Typing
- P1 Coercion: CastOp JIT + IR, `(cast expr : Type)` ✅
- P2 Bidirectional: `(check expr : Type)`, TypeAnnotation ✅
- P3 Type Language: `(: name Type)`, type-of, blame labels, type query ✅
- P4 Occurrence: predicate narrowing (string? → String, number? → Int) ✅

---

## 待启动

### D3: 自举 (40h)
Aura 编译器用 Aura 写。等前面稳定后再启。

### 已完成 (最新)
- **C FFI**: `c-load`/`c-func` — dlopen/dlsym, Int/Float/String/Opaque marshalling, JIT symbol API

### Phase E: Intent Orchestration — 高层意图编排

**估算：8-12 天**

目标：从
"LLM 一次写对"到"系统通过迭代达成目标"。添加 `(intend goal strategy: name)` 内置原语，
自动将高层意图拆解为 EDSL 管线 + 错误修正循环。

详情：[intent orchestration design](design/intent_orchestration.md)

#### E1: intend 原语 (2-3d)
- 注册 `(intend goal ...)` 为内置原语
- 实现 `generate-and-fix` 策略（等价于当前 `--fix` 循环）
- 返回 `#(status code iterations timeline)` record
- 测试：`tests/test_intent.aura`

#### E2: Strategy 系统 (3-5d)
- `define-strategy` 宏 → 策略展开为可 mutate 的 strategy record
- 内置策略：generate-and-fix, error-feedback-loop, refactor, optimize
- Timeline 记录 + `(intend-history)` 查询

#### E3: 集成 (2-3d)
- `edsl_benchmark.py` 的 `--fix` 循环用 `intend` 替换
- Benchmark 报告包含 iteration 数、strategy 名、timeline
- 评测从"代码正确率"变成"意图达成率"

#### E4: 可演化策略 (远期)
- LLM 通过 `(mutate:rebind "strategy" new-def)` 修改策略
- 多意图协作：`(intend A)` 内部调 `(intend B)`
- 意图树可视化

---

## 短期改善 (1-3h/each)

- JIT EvalValue 兼容: Bool/Pair/String 正确编码 → auto-JIT 覆盖全量
- stdlib 补全: json/validate/struct 生产级
- `--serve` AI agent 优化
- FFI: JIT 符号表集成 → 零开销 C 调用
- stdlib 加 `frequencies` 原语（减少 LLM 手写 word-freq 的复杂度）

---

## 已交付

| 日期 | 交付物 | 详情 |
|------|--------|------|
| 05-14 | FlatAST SoA | 内存布局优化, SoA → AoS 访问 |
| 05-15 | Typed Mutation | 类型化变异日志, AST-level mutation API |
| 05-16 | Arena + Compact GC | 42% 内存节省, 简单 GC |
| 05-17 | Sound Gradual Typing | Coercion + occurrence + blame |
| 05-18 | LLVM ORC JIT v2 | 38 opcode native, 闭包/Pair/CastOp |
| 05-19 | C FFI | dlopen/dlsym, marshalling, JIT symbol |
| 05-20 | EDSL Agent Benchmark | 17→24 stable, 多轮聚合+迭代修正 |
| 05-20 | Intent Orchestration Design | intend 原语设计文档 |
