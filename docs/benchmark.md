# EDSL Benchmark

135 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、所有权、合成管线。

自适应迭代修正（intend 模式，最多 3 次重试 + ant colony 零 LLM 变异修复）。

## 最新结果 (2026-05-28)

| 模型 | 通过 | 通过率 | 耗时 |
|:----|:----:|:-----:|:----:|
| Grok (xAI) | **135/135** | **100%** | ~2min |
| DeepSeek v4 Flash | 103-106/135 | 76-79% | ~10-25min |
| MiniMax M2.7 | 29/135 | 21.5% | ~8min |

> Grok 首次生成通过率 ~22%，ant colony 零 LLM 开销修复剩余 78%。

## 运行

```bash
# Aura-native（推荐）
LLM_API_KEY="***" BENCH_LIMIT=10 ./build/aura < tests/bench.aura
LLM_API_KEY="***" bash tests/run_parallel.sh 6

# Python runner
LLM_API_KEY="***" python3 tests/edsl_benchmark.py
```

## 历史

| 日期 | 版本 | 通过率 (Grok) | 通过率 (DS) |
|:----|:----:|:------------:|:-----------:|
| 2026-05-28 | 双 Arena / Grok 全量通过 | **100%** | 76-79% |
| 2026-05-27 | 全链路优化 | 80-84% | 74-76% |
| 2026-05-26 | Phase 5 自托管 | 81.5% | 74.8% |
| 2026-05-24 | P1 加固 | 83.8% | 82.9% |
