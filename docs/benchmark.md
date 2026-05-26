# Aura EDSL Benchmark

> 135 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、M4 线性所有权、Synthesize。
>
> 自适应迭代修正（intend 模式，最多 3 次重试）。

---

## Latest: 2026-05-26 — Phase 5 自托管改造（P0-P3 完成）

**本周变（10 commits, ~20 files）：**
- `http-post` pipe+fork→libcurl C API（dlopen 运行时加载）
- Phase 5 三阶段 roadmap（docs/roadmap.md）
- `run-one` 改用 `intend` 控制器（generator / verifier / fixer）
- ant colony 局部变异修复器（fine/putt phase 不调 LLM）
- `check_success` 灵活 substring 匹配（替代严格 `string=?`）
- TASK_HINTS 注入 system prompt
- 裁剪 76 行 API ref → 简洁 2 行 prompt
- 并行执行（`run_parallel.sh`, 4 workers）
- crash 修复：目录 IO、JIT 符号、arena OOM、if_false
- `drop` 改名为 `skip`（避免 parser 特殊形式冲突）

| 模型 | 任务数 | 通过 | 通过率 | 耗时 |
|:----|:-----:|:----:|:-----:|:----:|
| 🥇 **Grok 4.3** | 135 | **110** | **81.5%** | ~47s |
| 🥈 **DeepSeek v4 Flash** | 135 | **101** | **74.8%** | ~227s |

### 新增通过（vs 前次 Grok 83.7%/DS 72.6%)

| 任务 | Grok | DeepSeek | 原因 |
|:-----|:----:|:--------:|:------|
| edsl-defuse-cross | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-defuse-multi | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-require-stdlib | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-workspace-cow | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-snapshot-multi | — | ❌→✅ | hint 修复 |
| type-let-poly-hof | ❌→✅ | — | `(: x Type val)` 绑定修复 |
| type-grad-multi-boundary | ❌→✅ | — | API ref / 绑定修复 |
| type-linear | ❌→✅ | — | API ref / 绑定修复 |
| type-coercion-chain | ❌→✅ | ❌→✅ | API ref / 绑定修复 |
| ffi-sqrt | — | ❌→✅ | API ref c-func 签名修正 |
| ffi-strlen | — | ❌→✅ | API ref c-func 签名修正 |
| table-lookup | — | ❌→✅ | API ref / hint 修正 |

### 共同失败（19 个 — 纯 LLM 生成质量）

| 任务 | 失败原因 |
|:-----|:----------|
| `binary-search` / `merge-sort` | LLM 生成 closure 后不调用 |
| `ffi-sqrt` (Grok) | 类型签名错误（`Int` vs `Float`） |
| `edsl-rule` / `rule-basic` | LLM 不会用 `rule:define` 的 keyword API |
| `edsl-messaging` | send/recv 协议理解偏差 |
| `edsl-pipeline-basic` / `synthesize-pipeline` | 管线语法复杂 |
| `edsl-splice-wrap` / `mutation-rollback` | mutate 节点 ID 偏移 |
| `edsl-optimize-multiarg` | 多参优化调用格式错误 |
| `adt-*/adt-tree` | LLM 不熟 ADT 语法 |
| `type-occ-*` / `type-linear-hof` | 复杂类型系统场景 |
| `word-freq` | 用错 stdlib 函数名 |

0 个编译器 bug。

## Run Yourself

```bash
# 全部 135 任务 × 双模型
python3 tests/run_bench_all.py --parallel

# 单模型
LLM_API_KEY="$(cat ~/code/keys/grok)" \
  LLM_MODEL="grok-4.3" \
  LLM_BASE_URL="https://api.x.ai/v1" \
  python3 tests/edsl_benchmark.py --max-attempts 3
```

## History

| 日期 | 版本 | 任务数 | Grok | DeepSeek | 说明 |
|:----|:----:|:-----:|:----:|:--------:|:------|
| 2026-05-26 | Phase 5 | 135 | **81.5%** | 74.8% | 自托管改造（P0-P3 完成） |
| 2026-05-26 | Phase 4 | 135 | **83.7%** | 72.6% | 全管线完成 + 3 bug fix |
| 2026-05-26 | P2 全部 | 135 | 82.2% | 80.7% | hint 修复 + 编译器 bug |
| 2026-05-24 | P1 加固 | 111 | 83.8% | 82.9% | M4/closure/occurrence |
| 2026-05-23 | 早期 | 102 | 91.2% | 85.3% | 任务少、通过率虚高 |

> DeepSeek 的通过率波动较大（±7%），受 LLM 方差影响。建议多轮跑分后取均值。

---

## Self-Hosted Benchmark (`tests/bench.aura`)

Aura-native 版 benchmark，不走 Python，全在 Aura 内运行。

| 指标 | Python runner | Aura-native | 差异原因 |
|:-----|:-------------|:------------|:--------|
| 全量耗时 | ~3 min | ~15 min | Python 用 20 workers；Aura 4 workers |
| 单次过率 | 101/135 (74.8%) | ~10% | prompt 裁剪、check_success、TASK_HINTS 等仍需同步 |
| HTTP | `http.client` | libcurl C API（popen） | ✅ 已同步 |
| Code extraction | 6 步 fallback | ` ```lisp ` → define/display | ✅ 已同步 |
| Retry | intend + ant colony | intend + ant colony | ✅ 已同步 |
| Prompt | 2 行简洁 | 2 行简洁 | ✅ 已同步 |
| check_success | substring 匹配 | substring 匹配 | ✅ 已同步 |
| 并行 | 20 workers | 4 workers (shell) | ✅ 已有 |

### 已完成（Phase 5 P0-P3）
- [x] `http-post` libcurl C API（dlopen 运行时加载）
- [x] pipe+fork curl CLI 降级兜底
- [x] `intend` 控制器（generator/verifier/fixer）
- [x] ant colony 局部变异修复
- [x] `check_success` 灵活匹配
- [x] TASK_HINTS 注入
- [x] 简洁 system prompt（去掉 76 行 API ref）
- [x] 并行执行（`run_parallel.sh`）
- [x] LLM_BASE_URL 支持

### 待做
- [ ] SIGSEGV at ~119 tasks（`docs/issues/`）
- [ ] `--serve-async` multi-session 模式
- [ ] JSON 结构化输出
- [ ] 回归监控
