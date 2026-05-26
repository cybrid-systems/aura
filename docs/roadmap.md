# Aura 路线图

**更新：2026-05-26 — P0-P5 完成，Phase 5 规划中**

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

### 根因（与 Python runner 对比）

| 维度 | Aura-native | Python | 影响 |
|------|------------|--------|------|
| HTTP 调用 | curl CLI + temp 文件 | `http.client` epoll 复用 | 速度差 100x |
| 并行度 | 串行 for 循环 | 20 workers ThreadPool | 速度差 20x |
| curl 超时 | 无 | 15s per call | 无限挂死 |
| Code extraction | 只认 ` ```lisp ` | 多格式 fallback | 过率差 |
| Task hints | 静态全量 API ref | 按任务裁剪 prompt | 过率差 |
| 迭代修正 | 单次 shot | intend + PID adaptive | 过率 7% vs 73% |

### P0 — 基础设施（当前）
- [x] `http-post` 加 curl 超时（`--max-time 30`）
- [x] `extract-code` 加 `(define ...)` / `(display ...)` fallback
- [ ] bench.aura 改用 `--serve` 模式（持久化 workspace）
- [x] `http-post` 改为 pipe+fork execvp（不走 temp 文件 + shell）

### P1 — 智能调度
- [x] `run-one` 加 retry 循环（失败喂回 LLM）
- [x] TASK_HINTS 从 bench-tasks.json 注入 system prompt
- [ ] 错误诊断传播（不吞空字符串）
- [ ] 并行执行（serve 多 session）

### P2 — 自托管闭环
- [ ] 结构化 JSON 结果输出（兼容 `bench_results/*.json`）
- [ ] 完整 intend 修正管线替代 call_adaptive
- [ ] 回归监控（git commit + 模型 + 过率）
- [ ] 原生 worker pool（替代 ThreadPoolExecutor）
