# EDSL Benchmark

149 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、functor、所有权、合成管线。

自适应迭代修正（intend 模式，最多 3 次重试）。无 ant colony 辅助。

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

