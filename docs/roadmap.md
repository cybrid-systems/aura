# Aura 路线图

**更新：2026-05-26 — Phase 5 P0-P3 完成**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites 通过 |
| EDSL Benchmark (Grok) | 110/135 (81.5%) |
| EDSL Benchmark (DeepSeek) | 101/135 (74.8%) |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Benchmark 自托管 | ⚠️ `tests/bench.aura` — 存在但 pass rate 差距大（~10% vs Python 75-81%） |

## Issue

- [ ] SIGSEGV at edsl-synthesize-pipeline (~119 tasks) → `docs/issues/crash-sigsegv-edsl-synthesize-pipeline.md`

## Phase 5 — 自托管 Benchmark 性能达标

### 已完成（P0-P3）
- [x] `http-post` 加 curl 超时 + pipe+fork execvp（不走 temp 文件 + shell）
- [x] `http-post` 改用 libcurl C API（dlopen 运行时加载，零子进程）
- [x] `extract-code` 加 `(define ...)` / `(display ...)` fallback
- [x] `run-one` 改用 `intend` 控制器（generator/verifier/fixer）
- [x] ant colony 局部变异修复
- [x] `check_success` 灵活 substring 匹配
- [x] TASK_HINTS 从 bench-tasks.json 注入 system prompt
- [x] 简洁 system prompt（去掉 76 行 API ref）
- [x] 并行执行（`run_parallel.sh`, BENCH_OFFSET）
- [x] `LLM_BASE_URL` / `LLM_MODEL` 环境变量支持
- [x] crash 修复：目录 IO、JIT 符号、arena OOM、`if_false`、`drop`→`skip`

### 待做
- [ ] **Serve 模式**：bench.aura 改用 `--serve-async` 持久化 workspace
- [ ] **错误诊断传播**：不吞空字符串
- [ ] **JSON 结果输出**：兼容 `bench_results/*.json` 格式
- [ ] **回归监控**：git commit + 模型 + 过率
