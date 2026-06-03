# EDSL Benchmark

148 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、functor、所有权、合成管线。

自适应迭代修正（intend 模式，最多 3 次重试）。无 ant colony 辅助。

## 最新结果 (2026-05-29)

System prompt 包含 Aura 语法参考（~1900 chars），代码提取支持 `(:` 类型注解模式。

| 模型 | 通过 | 通过率 | 类型 Task | 耗时 |
|:----|:----:|:-----:|:---------:|:----:|
| **Grok (xAI)** | **121/148** | **83%** | **25/29 (86%)** | **60s** ⚡ |
| DeepSeek v4 Flash | 108/148 | 74% | 22/29 (76%) | 139s |
| MiniMax M2.7 | 34/148 | 23% | 0/29 (0%) | 84s |

> 改前：Grok 0%, DeepSeek 62%。提升关键在于：1) System prompt 补充完整语法参考 2) extract_code() 添加 `(:` 关键词支持类型注解提取。

## 运行

```bash
# 单模型（推荐）
python3 tests/run_bench_all.py --model grok
python3 tests/run_bench_all.py --model deepseek
python3 tests/run_bench_all.py --model minimax

# 全模型并行
python3 tests/run_bench_all.py --parallel

# Python runner（自定参数）
LLM_MODEL=grok-4.3 LLM_API_KEY="***" python3 tests/edsl_benchmark.py
```

## 历史

| 日期 | 版本 | 变化 | Grok | DeepSeek | MiniMax |
|:----|:----|:----|:----:|:--------:|:-------:|
| 2026-05-29 | 语法参考 + 提取修复 | 新增 10 个 task + type `:` 支持 | **83%** (改前 0%) | **74%** (改前 62%) | 23% |
| 2026-05-28 | 135 tasks + model routing | 基准线 | — | 76-79% | 21.5% |
