# Aura — 路线图

**更新：2026-05-20** — E4 Phase 1-3 完成 + 闭环。47/47 全过。let/closure 悬空指针修复。llm-fuzz 设计文档。

---

## 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 10/10 | TW + IR 双路径 + 显式调用栈 |
| **类型系统** | 🟢 10/10 | Sound Gradual + coercion + occurrence + let-poly + type query + blame |
| LLVM JIT | 🟢 10/10 | ORC JIT, 38 opcode native, -O2, 增量 cache, 闭包/Pair/PrimCall |
| 编译器基础设施 | 🟢 9/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / IR import |
| 测试覆盖 | 🟢 9/10 | integ 87 + unit 74 + smoke 5 + bash 117 + bench 44 + fuzz + regression 4 |
| 标准库 | 🟢 8/10 | 19 文件 ~1k 行 |
| 错误处理 | 🟢 9/10 | try/catch IR + diag + AST validate |
| EDSL / AI Agent | 🟢 10/10 | set-code/query/mutate/typecheck + LLM pipeline + iter correction |
| 文档 | 🟢 10/10 | README + tutorial + design repo + intent orchestration design |

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

#### E1: intend 原语 (2-3d) ✅
- 注册 `(intend goal [max-attempts])` 为内置原语
- generate-and-fix 循环：LLM 生成 → parse + eval_flat → 报错修正 → 重试
- 通过 curl + JSON API 调 LLM，支持 env var 配置（LLM_API_KEY / LLM_MODEL / LLM_BASE_URL）
- 返回 `"#(status:... goal:... iterations:...)"` 字符串
- 7 个生命周期测试：优雅失败、边界参数、EDSL 管线集成
- 无 API key 时不崩溃，返回失败记录

#### E2: Strategy 系统 (3-5d)
- `define-strategy` 宏 → 策略展开为可 mutate 的 strategy record
- 内置策略：generate-and-fix, error-feedback-loop, refactor, optimize
- Timeline 记录 + `(intend-history)` 查询

#### E3: 集成 (2-3d) ✅
- `edsl_benchmark.py` 的 `--fix` 循环用 `intend` 替换
- Benchmark 报告包含 iteration 数、strategy 名、timeline
- 评测从"代码正确率"变成"意图达成率"

#### E4: 可演化策略 ✅
- [设计文档](design/e4_evolvable_strategies.md)
- Phase 1 ✅: 结构化 intend-history + intend-analytics 原语
- Phase 2 ✅: strategy-field / strategy-set-field! / strategy-inspect
- Phase 3 ✅: evolve-strategy + benchmark --evolve 模式
- 闭环 ✅: evolved hints 注入下一轮 system prompt
- Phase 4: 多意图协作与意图树（远期）

---

## 短期改善 (1-3h/each)

- JIT EvalValue 兼容: Bool/Pair/String 正确编码 → auto-JIT 覆盖全量
- stdlib 补全: json/validate/struct 生产级
- FFI: JIT 符号表集成 → 零开销 C 调用
- 验证器升级：不只验代码能跑，还要验输出匹配期望值
- `--intend` 多轮聚合：`--rounds N` 在 intend 模式输出稳定度报告
- 扩 benchmark: 加入 LeetCode 风格任务，覆盖更多能力域

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
| 05-20 | E1: intend 原语 ✅ | (intend goal [max-attempts]) + 7 tests |
| 05-20 | E2: strategy system ✅ | define-strategy + timeline + intend-history |
| 05-20 | E3: benchmark integration ✅ | --intend flag in edsl_benchmark.py |
| 05-20 | E3b: --intend 26/26 ✅ | JSON预转义修复递归栈溢出 |
| 05-20 | json-encode 原语 | Aura → JSON 序列化，支持 Int/Float/String/Bool/Void/List/Hash |
| 05-20 | json-get-string 原语 | JSON 字符串字段提取（轻量版，不解析完整树） |
| 05-20 | json-parse 原语 | JSON → Aura 解析，null/true/false/number/string/array/object 全支持 |
| 05-20 | 动态 generator/fixer | --intend 模式用 json-encode + json-get-string 代替静态预转义 body |
| 05-20 | frequencies stdlib | 一行统计列表频次，hash-stats/word-freq 任务直接调用 |
| 05-20 | 扩 benchmark 到 47 任务 | +13 中难度 +8 高难度（quicksort/sieve/memoize/compose-n/...） |
| 05-20 | llm-fuzz 设计 | docs/design/llm_fuzz_testing.md |
| 05-20 | let/closure 悬空指针修复 | memoize 任务 0/1 → 47/47 全过 |
| 05-20 | fuzz Phase 1-2 | tests/test_fuzz.py + regression CI (4 个已知 bug) |
| 05-20 | fuzz Phase 3 | coverage-report 原语 + 9 路径编译器埋点 |
| 05-20 | E4 Phase 3: evolve-strategy | lib/std/evolve.aura + benchmark --evolve |
| 05-20 | E4 Phase 2: strategy-field/set-field!/inspect | 策略字段读写原语 |
| 05-20 | E4 Phase 1: intend-analytics | 结构化历史 + 错误分类 |
| 05-20 | E4 设计文档| 05-20 | E4 设计文档 | docs/design/e4_evolvable_strategies.md — Phase 1-4 方案 |
