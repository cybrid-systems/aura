# Aura 路线图

**更新：2026-05-26 — Phase 5 进行中**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites 通过 |
| 编译器 Bug | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Benchmark 自托管 | ⚠️ `tests/bench.aura` — 存在但性能差（~2min/task vs Python 3min/135task），过率 7% vs 55% |

## 已完成

### 编译器 Bug 修复
- 类型标注 binding, FFI closure dispatch, pipe mode 报错, if-no-else 条件求值, rest-arg 空参
- 常量文件夹 tagged bool (AOT 兼容), if_false 测试预期

### AOT 深入修复
- Fixnum/布尔 tagged 值 (OpMul/Div/Eq/And/Or/Not/Branch)
- string 显示 STRING_BIAS, NumberToString fixnum 解码
- runtime.c IS_PAIR 替换 val<0 (修复 apply/reverse/range/unique)
- Float 显示 + float pool + 算术 (OpAdd/Sub/Mul/Div)
- 常量文件夹 tagged bool 传播修复 (IS_TRUTHY)

### 统一值表示
- EvalValue 从 std::variant<16> 改为 int64_t pointer tagging
- AOT ↔ evaluator 零转换 — filter/map/= 正确工作
- 修复 permutations segfault (P4)

### 内建 benchmark
- 135 tasks 从 JSON 加载, synthesize:test-driven 管线
- 多轮 + 结果聚合 + 表格输出 (P5)

## Phase 5 — 自托管 Benchmark 性能达标

### 背景
`tests/bench.aura` 跑全量 135 任务比 Python `edsl_benchmark.py` 慢 ~100x（~2h vs ~3min），单次通过率 7% vs 55%。

### 已完成
- [x] `http-post` 加 curl 超时（`--max-time 30`）
- [x] `extract-code` 加 `(define ...)` / `(display ...)` fallback
- [x] `http-post` 改为 pipe+fork execvp（不走 temp 文件 + shell）
- [x] `run-one` 改用 `intend` 控制器 + retry
- [x] TASK_HINTS 从 bench-tasks.json 注入 system prompt

### Issue
- [ ] SIGSEGV at edsl-synthesize-pipeline (~119 tasks) → `docs/issues/crash-sigsegv-edsl-synthesize-pipeline.md`

### 待做
- [ ] **P0 — Serve 模式**：bench.aura 改用 `--serve` 持久化 workspace
- [ ] **P1 — 并行执行**：serve 多 session + send/recv 并发跑任务
- [ ] **P1 — 错误诊断传播**：不吞空字符串
- [ ] **P2 — JSON 输出**：兼容 `bench_results/*.json` 格式
- [ ] **P2 — 回归监控**：git commit + 模型 + 过率
- [ ] **P3 — 原生 HTTP**：去掉 curl 子进程（需 TLS 支持）
