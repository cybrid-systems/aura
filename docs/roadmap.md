# Aura — 路线图

**我们正在把什么变成现实。**

Aura 的路线图不是特性列表——是把「代码可以自己进化」这个想法一步步变成工程现实的记录。

**更新：2026-05-22**

---

## 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 10/10 | TW + IR 双路径 + 显式调用栈 |
| **类型系统** | 🟡 7/10 | inference + occurrence 扎实；coercion 全路径未闭环，ADT 缺失 |
| LLVM JIT | 🟢 10/10 | ORC JIT, 38 opcode native, -O2, 增量 cache, 闭包/Pair/PrimCall |
| 编译器基础设施 | 🟢 9/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / IR import |
| 测试覆盖 | 🟢 10/10 | integ 87 + unit 74 + smoke 5 + bash 117 + bench 57 + fuzz + regression 4 |
| 标准库 | 🟢 8/10 | 19 文件 ~1k 行 |
| 错误处理 | 🟢 9/10 | try/catch IR + diag + AST validate；**新：** 错误消息自我赋值 bug 已修 |
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

**Phase D2a 已完成（骨架，可进一步强化）：**

| 组件 | 进度 | 说明 |
|------|:----:|------|
| TypeRegistry + TypeId | 90% | TypeTag、instantiate、format_type、is_subtype |
| Inference (synthesize/check) | 85% | 双向推断、let-poly、arithmetic specialization |
| Occurrence typing | 80% | predicate narrowing + if branch refinement |
| Constraint solving | 60% | 单遍 solve，无 union-find，variadic → Dynamic |
| Coercion insertion | 40% | CastOp 仅覆盖 IR/JIT，tree-walk 缺失；coercible 白名单小 |
| Runtime blame | 50% | JIT 有 blame，解释器/IR 弱 |
| Module type checking | 0% | import/require 全部 Dynamic |
| ADT (Variant/Record) | 0% | 无 sum type、无模式匹配 |
| Parametric container | 0% | 无 `(List Int)` 语法 |

---

## 待启动 (按优先级排序)

### Phase T: 类型系统完善 — Soundness + 表达力

**当前是主攻方向。** 类型系统的 inference + occurrence 已经扎实，但 coercion 未全路径闭环，ADT 缺失限制了 Agent 数据建模能力。

**估算：3 周**

#### T1: Soundness 闭环（本周）

| 任务 | 涉及文件 | 估算 |
|------|---------|:----:|
| CoercionInsertionPass 全路径覆盖（Call 返回/if 分支/Pair 等） | `pass_manager.ixx` | 1d |
| 树遍历 eval 器 CastOp 执行器 | `evaluator_impl.cpp` | 0.5d |
| 扩展静态 coercible 类型对（Float↔Int, Bool↔Int） | `type_impl.cpp` | 0.5d |
| 运行时 blame 信息增强 | `ir_executor_impl.cpp`, `aura_jit_runtime.cpp` | 0.5d |
| 混合类型测试：`(+ 1 "2")` 不静默返回 `"12"` | `tests/typecheck/` | 0.5d |

**验收：** 所有执行路径对类型错误行为一致

#### T2: ADT + 结构类型（下周）

| 任务 | 涉及文件 | 估算 |
|------|---------|:----:|
| Variant 类型标签 + 构造语法 | `ast.ixx`, `parser_impl.cpp`, `type.ixx` | 2d |
| `(match x (case (Some v) ...) (case (None) ...))` | `parser_impl.cpp`, `type_checker_impl.cpp` | 2d |
| 参数化容器 `(List Int)` | `type.ixx`, `parser_impl.cpp` | 1d |
| ADT 测试：Option、Tree、穷尽性检查 | `tests/typecheck/` | 1d |

**验收：** `(define-type (Tree a) (Leaf a) (Node Tree Tree))` 能定义和使用

#### T3: 健壮性 + 开发者体验（第 3 周）

| 任务 | 涉及文件 | 估算 |
|------|---------|:----:|
| 约束求解优化（Union-Find + 多遍 fixpoint） | `type_checker_impl.cpp` | 2d |
| 模块 import 类型签名 | `service.ixx`, `type_checker.ixx` | 1d |
| query-and-fix 自动 cast 插入 + 类型注解建议 | `query_impl.cpp` | 1d |
| 类型错误信息人类可读增强 | `type_checker_impl.cpp`, `diag.ixx` | 1d |

**验收：** 模块导入类型不再是 Dynamic；query-and-fix 能补缺失注解

### D3: 自举 (40h)
Aura 编译器用 Aura 写。等类型系统稳定后再启。

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

### 自适应 Intend + 蚁群控制器 (PID 控制 ✅)

**核心思想**：高尔夫隐喻 + 控制理论 + 蚁群算法。
LLM 做方向指引（蚁后），Aura EDSL 做局部搜索（工蚁），距离反馈做信息素。

#### Phase 1: 距离度量 + 结构化诊断 ✅
- [x] `measure-distance()` — rc + 输出匹配率 → phase (coarse/fine/putt)
- [x] `structured-diagnosis()` — 输出特征诊断 + missing-kw diff
- [x] `current-source` 捕获 → LLM 能看到自己写的代码
- [x] 回路闭环：3 次 retry 带结构化反馈 + `<hash[N]>` 告警

#### Phase 2: 两阶段 prompt 切换 ✅
- [x] coarse / fine 自适应 temperature/tokens 控制
- [x] API Reference 注入 (get-api-ref 覆盖 6 个模块)
- [x] Common Pitfalls (display hash, modulo vs mod, > vs >=)
- [x] Execution trace 注入 (primes-list/quicksort/prime-test/tcp-connect)

#### Phase 3: 蚁群控制器 (Python 级) ✅
- [x] `internal_colony_search()` — Python 端局部变异搜索
- [x] `_find_mutables()` — 扫描代码表面的数值/操作符/display/函数
- [x] `_gen_variants()` — 生成 20+ 种局部变体
- [x] PID 集成：fine/putt 时先殖民地搜索，失败再 LLM
- [x] 安全限制：每变体 3s 超时，总共 8s 限时，20 变体上限
- [x] Scheme 兼容层：first/rest/cadr/odd?/even? 等 serve 预定义

#### Phase 4: 蚁群控制器 (Aura 级) — 设计完成，待实现
- [ ] EDSL 化变异：`set-code + mutate:* + eval-current` 代替字符串替换
- [ ] `lib/std/ant.aura`：信息素系统 + 节点扫描 + 变异生成
- [ ] `colony:search()`：纯 Aura 搜索循环，1 次 IPC 代替 20 次
- [ ] PID 裁剪：按信息素排序 + 动态限制搜索深度
- [ ] 跨任务信息素持久化：`pheromone:export/import`
- [ ] 预期：每变体成本从 20ms → <1ms，搜索容量从 20 → 1000+

### 当前 Benchmark 结果 (2026-05-22)

**条件：** `max-attempts=3`，无 Scheme 兼容层（着力即差），`--json`

| 模型 | 通过率 | 耗时 | 失败 |
|------|:-----:|:----:|:----|
| **Grok 4.3** | **57/57 (100%)** 🎯 | ~9.6min | — |
| **DeepSeek v4 Flash** | **54/57 (94.7%)** | ~10.4min | ffi-sqrt, ffi-strlen, edsl-set-code |
| **MiniMax-M2.7** | **~53/57 (~93%)** 🏃 | ~15min | (running) |

**较上轮提升：** DeepSeek 51→54（+3），MiniMax 45→~53（+8）。
Scheme 兼容层移除 + 编译器错误消息修复后，模型被迫写纯正 Aura 代码而非 Scheme 习气，pass_rate 显著上升。

**已修复的编译器报错：**
- `<closure[281474976710656]>` → `#<procedure>`（闭包显示人类可读）
- `unbound variable: `（空名字）→ 显示正确变量名 + 建议
- 根因：`d = std::move(d).with_suggestion(...)` 自我赋值导致消息被清空

---

## 短期改善 (1-3h/each)

- EDSL 化殖民地搜索：`set-code + mutate:* + eval-current` 管道
- `lib/std/ant.aura`：信息素表 + 节点扫描 + colony:search
- 跨任务信息素持久化
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
| 05-20 | E1: intend 原语 | (intend goal [max-attempts]) + 7 tests |
| 05-20 | E2: strategy system | define-strategy + timeline + intend-history |
| 05-20 | E3: benchmark integration | --intend flag in edsl_benchmark.py |
| 05-20 | E4: evolve-strategy | lib/std/evolve.aura + benchmark --evolve |
| 05-20 | 自适应 PID Intend | 55/57, trace feedback + API ref injection |
| 05-21 | **extract_code 修复** | **57/57 (100%)** — 双模型全通过 |
| 05-21 | **蚁群控制器 (Phase 1)** | local mutation search + PID integration |
| 05-21 | **Scheme 兼容层** | first/rest/cadr/odd?/even? serve 预定义 |
| 05-21 | **serve 性能优化** | sleep(1)→sleep(0.05), exec 12x 加速 |
| 05-22 | **移除 Scheme 兼容层** | 着力即差 — 删 12 条 serve 注册 + 4 条字符串替换 |
| 05-22 | **编译器报错修复** | `<closure[N]>` → `#<procedure>`；`with_suggestion` 自我赋值 bug；短变量名显示 |
| 05-22 | **Grok 57/57 (100%)** | 全量 benchmark 首次满通过 |
| 05-22 | **类型系统路线图** | 3 周开发计划：Coercion 闭环 → ADT → 求解器优化 |
