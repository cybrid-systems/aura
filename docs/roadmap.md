# Aura — 路线图（TODO）

**更新：2026-05-23**

**当前定位**：语言核心完备（Tree-walker + IR + JIT、Sound Gradual Typing、ADT、Linear Ownership），
已进入「让 AI 自治编辑更可靠」的迭代阶段。

**当前基准**：Grok 77/85（90.6%）、MiniMax 74/85（87.1%）、DeepSeek 72/85（84.7%）。
新增的 28 个类型系统任务（15 个）是主要拉分项，平均通过率 ~60%。

---

## ✅ 全部修复（2026-05-23）

### P0 — 编译器缺陷

| # | 问题 | Commit |
|---|------|:------:|
| 1 | Blame：`(+ 1 "hello")` 输出 stderr 错误 | `052cb19` |
| 2 | `eval_flat` 缺失 Linear/Move/Borrow/Drop 节点 | `50208da` |
| 3 | `(: x Int)` 无绑定正确报错 | `82dfaf4` |
| 4 | `(: name Type val)` 三参数解析 | `afe96fd` |
| 5 | `#<procedure>` 系统 prompt 警示 + hint 强化 | `f8166c7` |

### P1 — 功能缺口

| # | 问题 | Commit |
|---|------|:------:|
| 6 | 模块 import 类型签名（28 个 stdlib 模块 90+ 签名） | `fab6a13` |
| 7 | `let` 泛化（synthesize 路径已验证正常工作） | — |
| 8 | match 穷尽性检查（parser 元数据 + 构造器表 + type checker + 测试） | `de2c59d` |
| 9 | 树遍历器 CastOp 覆盖率 + blame 覆盖 | `2b24586` |
| 10 | M4 运行时违规检测（double-move / use-after-move / double-drop） | `6e3f78f` |

### P2 — 增强

| # | 问题 | Commit |
|---|------|:------:|
| 11 | 增量类型检查（已验证已实现） | — |
| 12 | `--inspect` 扩展（ir/closures/cache/typecheck/evaluator/pretty/cache-open） | `03be5f1` |
| 13 | Serve 超时熔断（30s async timeout） | `e259288` |
| 14 | M3 P2996 反射（P1306 递归序列化，支持嵌套 struct / 泛型 vector / array / enum） | `b07b5c6` |
| 15 | `(: name Type val)` parser 修复 | `afe96fd` |

---

## 🔄 开放 TODO

### P2

| # | 任务 | 预估 | 说明 |
|---|------|:----:|:-----|
| 16 | **Colony search 下沉到 Aura** — 当前 `internal_colony_search` 在 Python 侧做字符串替换 + subprocess。
  管道已就绪：`set-code + mutate:* + eval-current`。
   需要：翻译搜索循环为纯 Aura EDSL 调用，暴露更多 `mutate` 原语（insert/delete/transform）。
   收益：50-100× 搜索加速，支持 1000+ variants/轮。 | 1-2d | `lib/std/ant.aura` 设计已有 |
| 17 | **Pheromone 持久化** — 当前信息素在 Python dict 中，会话结束即丢。
   暴露 `pheromone:export` / `pheromone:import` 到 Aura 层，磁盘存储。
   数据结构简单（hash-of-vectors），跨会话记忆。 | 1d | 收益：跨任务学习 |
| 18 | **Richer query/mutate API** — `query:children`、`query:dependents`、`mutate:insert`、`mutate:delete`。
   P2996 反射已支持类型安全 AST 模式匹配，需暴露更多原语。 | 1-2d | 依赖项：P0 |

### P3 — 中远期

| # | 任务 | 预估 | 说明 |
|---|------|:----:|------|
| 19 | **Intent Orchestration Phase E4** — 多意图协作 + intent tree。设计已有，
    实现并行子 Agent + 结果合并。 | 3-5d | — |
| 20 | **PID 控制器原生化** — 把 Python 的自适应搜索深度 + 距离反馈搬到 Aura runtime。 | 1-2d | 设计已清晰 |
| 21 | **Serve 模式升级** — WebSocket/gRPC，降低 Agent 循环延迟。 | 2-3d | 当前 stdin JSON 够用 |
| 22 | **Python/JS SDK** — 封装 ServeClient + EDSL 生成器。 | 社区 | 生态 |
| 23 | **自举** — Aura 编译器用 Aura 写。类型系统稳定后。 | 中 | — |
| 24 | **多意图协作与意图树** — Phase 4 远期。 | — | — |

---

## Benchmark 基线（85 任务，max-attempts=3，1 轮，2026-05-23 PM）

| 模型 | 通过率 | 耗时 | 短板 |
|:----|:------:|:----:|:-----|
| 🥇 Grok 4.3 | **78/85 (91.8%)** | ~13min | algorithm (3), ffi-sqrt, type-annot-fn, type-blame-runtime |
| 🥇 DeepSeek v4 Flash | **77/85 (90.6%)** | ~46min | adt-option, algorithm (2), ffi (2), type-annot-fn, type-blame-runtime |
| 🥈 MiniMax M2.7 | **76/85 (89.4%)** | ~23min | adt-option, algorithm (2), ffi (2), json-roundtrip, tcp-connect, type (2) |

**本次提升（2026-05-23 compiler 修复后）：** Grok +1, MiniMax +2, DeepSeek +5（弱模型收益最大）。
类型系统任务从 ~60% 提升至 ~80%。

**三模型共享失败：** `binary-search`、`merge-sort`（`#<procedure>` 问题，task hint 已补充）。

## 当前规划优先级

1. **P0: Colony search 下沉到 Aura** — 收益最大，基础设施已就绪
2. **P1: Pheromone 持久化** — 低投入，跨会话记忆
3. **P1: Richer query/mutate API** — 依赖项

## 已知问题

详见 [known_issues.md](known_issues.md) — 当前无开放 issue。
