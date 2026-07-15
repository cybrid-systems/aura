# EDSL Benchmark

**目标**：测试 Aura 作为“AI 驱动自演化系统”的实际能力 —— LLM 只给方向，Aura 提供精确 EDSL 变异 + 测量 + 闭环反馈。

149 个任务，覆盖语法、stdlib、类型、FFI、EDSL 自修改、Workspace、ADT、functor、所有权、合成等。

使用 intend 风格自适应迭代（最多 3 次重试），体现控制论闭环哲学。

## 最新结果 (2026-06-04)

System prompt 包含 Aura 语法参考（~1900 chars），代码提取支持 `(:` 类型注解模式。E4 auto-tune 任务 (`auto-tune-max-attempts`) 已加入。

| 模型 | 通过 | 通过率 | 类型 Task | 耗时 |
|:----|:----:|:-----:|:---------:|:----:|
| **MiniMax-M3** | **118/139** | **85%** | — | **22s** ⚡ |
| Grok (xAI) | 121/148 | 83% | 25/29 (86%) | 60s ⚡ |
| DeepSeek v4 Flash | 108/148 | 74% | 22/29 (76%) | 139s |
| MiniMax M2.7 | 34/148 | 23% | 0/29 (0%) | 84s |

> MiniMax-M3 是目前最强的模型：85% 通过率，略超 Grok (83%)。M2.7 → M3 跳跃最大（23% → 85%）。任务集不严格可比（M3 跑 149 个，1 个因缺 `;; goal:` 被跳，记 139 评分；Grok/DeepSeek 评分于 148）。
>
> 改前：Grok 0%, DeepSeek 62%。提升关键在于：1) System prompt 补充完整语法参考 2) extract_code() 添加 `(:` 关键词支持类型注解提取。

## MiniMax-M3 失败任务（21/139）

| 类别 | 任务 |
|:-----|:-----|
| ADT | `adt-either`, `adt-tree` |
| EDSL mutate | `edsl-colony`, `edsl-messaging`, `edsl-mutate-chain`, `edsl-mutate-extract`, `edsl-pipeline-basic`, `edsl-snapshot-multi`, `edsl-splice-wrap`, `edsl-synthesize-pipeline`, `edsl-rule`, `edsl-rule-basic`, `edsl-set-code` |
| M4 所有权 | `linear-basic`, `m4-borrow-chain`, `type-ownership-linear` |
| Type | `type-consistency`, `type-functor-annot`, `type-linear-hof` |
| 算法 | `table-lookup`, `word-freq` |

M3 亮点：1 attempt 通过率最高，几乎不需要重试。

## 运行

```bash
# 单模型（推荐）
python3 tests/run_bench_all.py --model grok
python3 tests/run_bench_all.py --model deepseek
python3 tests/run_bench_all.py --model minimax
LLM_MODEL=minimax-m3 LLM_API_KEY="***" python3 tests/edsl_benchmark.py

# 全模型并行
python3 tests/run_bench_all.py --parallel

# Python runner（自定参数）
LLM_MODEL=grok-4.3 LLM_API_KEY="***" python3 tests/edsl_benchmark.py
```

## 历史

| 日期 | 版本 | 变化 | Grok | DeepSeek | MiniMax |
|:----|:----|:----|:----:|:--------:|:-------:|
| 2026-06-04 | E4 auto-tune task | +1 task, M3 跑分 | — | — | **M3: 85%** |
| 2026-05-29 | 语法参考 + 提取修复 | 新增 10 个 task + type `:` 支持 | **83%** (改前 0%) | **74%** (改前 62%) | M2.7: 23% |
| 2026-05-28 | 135 tasks + model routing | 基准线 | — | 76-79% | 21.5% |

## Self-Evolution Closed-Loop Reliability Suite (Issue #1463)

**目标**：补充（不替换）上面的 single-shot EDSL task benchmark — 测的是多轮闭环可靠性，不是单次任务成功率。

**当前状态 (Phase 1 stub)**：

- **入口**：`tests/benchmark_self_evolution.aura` — 加载模块 + 调 `(run-self-evolution-bench :happy-path 50)` 返回 JSON summary。
- **场景**：只实现了 `:happy-path`（驱动 `lib/std/agent.aura` 的 `auto-grow` 跑到收敛或 max-rounds）。其他 4 个场景（`:panic` / `:rollback` / `:long-mutation` / `:steal`）是 follow-up。
- **Metrics**：返回的 metrics 块是 placeholder（全 0 / heuristic success）。Decision Metrics contract 落地后才填真值（AC3 follow-up #1463.1）。
- **CI integration** (AC4)：follow-up #1463.4 — 需要先有 pass/fail 阈值定义。
- **不重复**：本 benchmark 与上面的 149 个 single-shot EDSL task 是互补关系，不是替代。

**驱动方式（未来）**：

```bash
# In CI (follow-up #1463.4):
(import "tests/benchmark-self-evolution")
(run-self-evolution-bench :happy-path 50)
; → JSON summary; pass/fail threshold defined later
```

**Follow-ups**：

- #1463.1 — 接 Agent Decision Metrics contract，metrics 块填真值
- #1463.2 — `:panic` / `:rollback` / `:long-mutation` / `:steal` 4 个场景
- #1463.3 — JSON summary schema 文档化 + C++ 解析 helper
- #1463.4 — CI 集成 + pass/fail 阈值（nightly + on relevant PRs）

Refs: #1463

