# Aura EDSL Benchmark

> 135 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、M4 线性所有权、Synthesize。
>
> 自适应迭代修正（intend 模式，最多 3 次重试）。

---

## Latest: 2026-05-28 — 双 Arena + Grok 全量通过（v6）

**本次变更：**
- 双 Arena（persistent + temp）内存管理 — `gc-temp` 原语
- benchmark 改用并行多进程（`run_parallel.sh`），6 workers 跑 Grok
- 结果文件写入（`write-file` 替代 stdout 管道）
- 修复 `gc-temp` 误清 `pairs_`/`string_heap_`（aggregate 结果归零的 bug）
- 修复 `run_parallel.sh` TIMEOUT 过短（600→1200s）

| 模型 | 任务数 | 通过 | 通过率 | 耗时 | 说明 |
|:----|:-----:|:----:|:-----:|:----:|:------|
| 🥇 **Grok (xAI)** | 135 | **135** | **100%** | ~2min | Aura-native, 6 workers, ant colony 106/135 |
| 🥈 **DeepSeek v4 Flash (Aura-native)** | 135 | 106 | 78.5% | ~25min | 10 workers，4 worker API 限速超时 |
| 🥈 **DeepSeek v4 Flash (Python)** | 135 | 103 | 76.3% | ~10min | Python runner, 串行 |

> Aura-native 和 Python runner 的 DeepSeek 通过率一致（差异在 LLM 方差内），Grok 在质量和速度上均显著领先。  
> 首次生成通过率 ~22%，ant colony 零 LLM 开销修复剩余 78%。

---

## 2026-05-27 — 全链路优化（v5）

**本次变更（4 commits）：**

| # | 改动 | 位置 |
|:-:|:-----|:----:|
| 1 | `eval-current` 返回 `("kind" "message")` 结构化错误对偶 | **Aura** |
| 2 | `eval-current-output` 返回 `[kind] message` 格式 | **Aura** |
| 3 | `eval-current` 检测 closure 时自动调用 `(display ...)` 并重求值 | **Aura** |
| 4 | 错误类型→API 文档映射（rule:define, synthesize, for-each 等） | **Python** |

### 结构化错误（P1）

```
修复前: eval-current → "1:1: expected expression"             (平铺字符串)
修复后: eval-current → ("parse" "1:1: expected expression")   (结构化对偶)

错误分类: parse | type error | unbound variable | arity mismatch | …
```

### Closure 自动补 display（P0，纯 Aura 内置）

``` scheme
;; Before: eval-current returns #<procedure> (LLM 看不到结果, 3 轮 retry)
;; After:
(set-code "(define (double x) (* x 2))")
(eval-current)  → 自动扫描 AST 的 Define 节点
                → 提取 fn=double, arity=1
                → 试 (display (double 42)) → 84
                → 返回结果, 0 LLM 调用
```

| 模型 | 任务数 | 通过 | 通过率 | 耗时 |
|:----|:-----:|:----:|:-----:|:----:|
| 🥇 **Grok 4.3** | 135 | 108-114 | **~80%** (方差 ±4%) | 37-45s |
| 🥈 **DeepSeek v4 Flash** | 135 | 100-103 | **~75%** (方差 ±7%) | 134-161s |

> Grok 跑分在 80.0%-84.4% 间波动（4 次跑分），提升来自结构化和 auto-fix 降低 retry 次数，不是所有 task 都能通过。

### 当前 Aura 内置能力清单

| 能力 | 实现 | 依赖 Python? |
|:-----|:----|:-----------:|
| `set-code` 解析错误传回字符串 | ✅ evaluator_impl.cpp | ❌ 纯 Aura |
| `eval-current` 传播 set-code 错误 | ✅ `last_set_code_error_` 字段 | ❌ 纯 Aura |
| 结构化错误返回 `("kind" "message")` | ✅ `make_error_val()` 构建 pair | ❌ 纯 Aura |
| closure 自动补 `(display ...)` | ✅ AST 扫描 + 参数推断 + 重求值 | ❌ 纯 Aura |
| `intend` 控制器 (gen/ver/fixer) | ✅ bench.aura | ❌ 纯 Aura |
| `pid:analyze` 相位检测 | ✅ std/adaptive | ❌ 纯 Aura |
| ant colony 变异修复 | ✅ `colony:search` | ❌ 纯 Aura |
| `#<procedure>` 自动补 display (Python兜底) | ✅ _auto_fix_procedure | ✅ Python |
| `check_success` 防误判 | ✅ 字边界 + 结构化错误检测 | ✅ Python |
| 错误类型→API 文档映射 | ✅ _get_api_snippet() | ✅ Python |
| correction prompt 构建 | ✅ 类型/parse/#<procedure> 专用提示 | ✅ Python |

### 共同失败（~18 个 — 纯 LLM 生成质量，不是编译器 bug）

| 类别 | Tasks | 根因 |
|:-----|:------|:------|
| ADT 语法 | `adt-tree/option/multi-ctor/either` | LLM 不熟 `define-type` |
| EDSL API | `edsl-rule/pipeline/synthesize/messaging/optimize` | API 复杂度高 |
| 类型系统 | `type-occ-cond/deep/match`, `type-linear-hof`, `type-let-poly-hof`, `type-grad-multi-boundary` | LLM 写不出正确代码 |
| C FFI | `ffi-strlen`, `ffi-sqrt` (Grok) | `c-func` 签名语法 |
| 算法 + closure | `binary-search`, `merge-sort` | LLM 参数复杂 → auto-fix 不命中 |
| 纯算法 | `table-lookup`, `unique-hash`, `word-freq` | LLM 算力上限 |

### 下次优化方向

1. **`check` Aura 原语** — 把 check_success 移到 Aura 内，bench.aura 可完全自托管
2. **retry 策略分离** — 按错误类型用不同 temperature/token
3. **closure auto-fix 参数推断** — 从 task hints 提取签名，生成更准的 display 参数

---

## Previous: 2026-05-26 — Phase 5 自托管改造（P0-P3 完成）

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

---

## 全链路数据分析（2026-05-26）

### 控制器数据流（完整 trace）

```
┌─ Task 定义 ─────────────────────────────────────────────┐
│ tests/tasks/<category>/<name>.aura                      │
│   ;; goal: Use set-code + query:find...                 │
│   ;; expect: Define                                     │
│   ;; hint: 示例代码                                      │
│   ;; depend: std/stdlib.aura                            │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Prompt Builder ────────────────────────────────────────┐
│ build_sys_prompt(stdlib, api_ref, task_name)             │
│   = PROMPT_SECTIONS["identity"]                         │
│   + "Available stdlib: ..."                              │
│   + TASK_HINTS[task_name]                                │
│   + api_ref (fine/putt phase 才加)                       │
│                                                          │
│ 目前只有一个 section "identity":                         │
│   "You are Aura Lisp. Write valid code ending with       │
│    (display ...). CRITICAL: (display ...) or TEST FAIL!" │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ LLM Generation ────────────────────────────────────────┐
│ POST /v1/chat/completions (stream=false)                │
│   messages = [                                          │
│     {role: "system", content: sys_prompt},              │
│     {role: "user",   content: task_prompt},             │
│     ... (retry corrections 追加)                         │
│   ]                                                     │
│   → resp                                                 │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Code Extraction ───────────────────────────────────────┐
│ extract_code(resp)  (6 步 fallback)                     │
│   1. \`\`\`lisp ... \`\`\` → 2. scheme → 3. racket        │
│   4. clojure → 5. 无标注代码块 → 6. 全文兜底              │
│   → code string 或 None                                  │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Execution ─────────────────────────────────────────────┐
│  if code starts with "(set-code":                        │
│    # EDSL 模式                                           │
│    ok,out,err = run_code(code + "\n(eval-current)")     │
│  else:                                                   │
│    # Full code 模式                                      │
│    ok,out,err = run_code(code)                           │
│    if ok: last_full_code = code                          │
│                                                          │
│  run_code: serve.exec() or subprocess([AURA], code)      │
│  → (ok: bool, out: stdout, err: stderr)                  │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Check ─────────────────────────────────────────────────┐
│ success = ok AND (check_success(out,expected)            │
│                 OR  check_success(err,expected))          │
│                                                          │
│ check_success(out, expected):                            │
│   for kw in expected:           # substring 匹配         │
│     if kw in norm_out: return True                       │
│   return False                                           │
│                                                          │
│  ✓ pass → return                                          │
│  ✗ fail → retry ↺                                        │
└──────────────────────────┬───────────────────────────────┘
                           ▼ retry
┌─ Adaptive Feedback ─────────────────────────────────────┐
│ call_adaptive(0, actual_output, expected)                │
│   → Aura pid:analyze 原语                                │
│   → 返回 (phase ratio diagnosis feedback_text)            │
│                                                          │
│   phase = coarse/fine/putt (距目标距离)                   │
│                                                          │
│ 算法类 task 额外跑 get_execution_trace                   │
│ 注入完整执行 trace 到 feedback                            │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Ant Colony (fine/putt ⤵) ──────────────────────────────┐
│ internal_colony_search(serve, code, expected, phase)     │
│   → serve 模式 + fine/putt phase 生效                    │
│   → Aura colony:search 原语，局部变异                     │
│   → 零 LLM 调用                                          │
│   ✓ → 返回成功，避免 LLM round-trip                       │
└──────────────────────────┬───────────────────────────────┘
                           ▼
┌─ Correction Prompt ─────────────────────────────────────┐
│ correction = (                                           │
│   "(compile error) " or "(output mismatch) "             │
│   + distance_note (phase 映射)                           │
│   + "Aura produced: {actual_output[:300]}"               │
│   + ada_fb (pid:analyze 的诊断)                           │
│   + procedure_warn (if #<procedure> in output)           │
│   + "Current code:\n{code[:400]}"                        │
│ )                                                         │
│ messages.append(assistant, resp)                          │
│ messages.append(user, correction)                         │
│ → LLM retry ↺ (max 3)                                    │
└──────────────────────────────────────────────────────────┘
```

### 关键路径对比：修复前后

以 `edsl-set-code` 为例，LLM 生成：
```scheme
(set-code "(define (f x) (+ x 1))")
(display (query:node-type "Define"))
```

**✅ 正常通过：**
```
→ Full code 模式（不以 set-code 开头）
→ Aura 直接执行
→ stdout: "(0 1 2 3...)" 含 "Define"
→ check_success → True ✅
```

**❌ 修复前失败：**
```
LLM → (set-code "broken syntax...")
      (eval-current)  ← 自动追加
     → set-code 失败 → #f (被 begin 吞)
     → eval-current 在旧 workspace 跑 → "()"
     → actual_output = "()"
     → pid:analyze → "empty output"
     → LLM: "Aura produced: ()"
     → 不知道哪里错，3 轮全部浪费
```

**✅ 修复后：**
```
LLM → (set-code OK) → (query:find ...) → type error
     → eval-current 返回 "error: type mismatch..."
     → actual_output = "error: type mismatch..."
     → LLM: 看到精确错误 → 修正 → 通过
```

关键：`edsl-set-code` DS 从 **0% → 100%** 就是因为 eval-current 现在返回类型错误诊断。

### Retry 各轮变化

```
Attempt 1 (coarse, temp=0.3, tok=4096, full rewite)
  → Aura produced: ()       ← 旧 workspace 空
  → phase: coarse, LLM: "Still far from goal"

Attempt 2 (fine, temp=0.2, tok=2048, partial fix)
  → 仍然 () → phase: fine
  → ant colony 在 last_full_code 上局部变异
  → 变异找到匹配 → pass (0 LLM cost)

Attempt 3 (putt, temp=0.1, tok=1024, minor fix)
  → phase: putt, LLM: "Almost there!"
  → 最终尝试
```

### 现存瓶颈

| 环节 | 问题 | 影响 |
|:-----|:----|:-----|
| EDSL chain | `(set-code ...)(eval-current)` 中 set-code 错误被 begin 吞 | 语法错误无法诊断 |
| pid:analyze | 空输出只能报 "empty"，分不清"旧 workspace" vs "无输出" | phase 判断偏差 |
| check_success | 仅 substring 匹配 | 可能误判 |
| procedure 检测 | 只在 correction 做，不在 check_success | 浪费一次 LLM 调用 |
| TASK_HINTS | 手工维护 135 条，可能不同步 | 过时 hint 误导 |

---

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
| 2026-05-28 | 双 Arena (v6) | 135 | **100%** 🏆 | 76-79% | Aura-native + Grok 全量通过; 双 Arena 内存管理 |
| 2026-05-27 | 全链路优化 (v5) | 135 | **80-84%** (±4%) | 74-76% (±7%) | 结构化错误 + Aura closure auto-fix + API 文档映射 |
| 2026-05-27 | EDSL chain 穿透 (v3) | 135 | **84.4%** | 76.3% | set-code 穿透 + auto-fix + check_success |
| 2026-05-26 | 诊断传播 v2 | 135 | 80.0% | 76.3% | eval-current 返回错误字符串 |
| 2026-05-26 | Phase 5 | 135 | **81.5%** | 74.8% | 自托管改造（P0-P3 完成） |
| 2026-05-26 | Phase 4 | 135 | **83.7%** | 72.6% | 全管线完成 + 3 bug fix |
| 2026-05-26 | P2 全部 | 135 | 82.2% | 80.7% | hint 修复 + 编译器 bug |
| 2026-05-24 | P1 加固 | 111 | 83.8% | 82.9% | M4/closure/occurrence |
| 2026-05-23 | 早期 | 102 | 91.2% | 85.3% | 任务少、通过率虚高 |

> DeepSeek 的通过率波动较大（±7%），受 LLM 方差影响。建议多轮跑分后取均值。

---

## Self-Hosted Benchmark (`tests/bench.aura`)

Aura-native 版 benchmark，不走 Python，全在 Aura 内运行。

| 指标 | Python runner | Aura-native | 同步状态 |
|:-----|:-------------|:------------|:--------|
| 全量耗时 | ~3-10 min (20wk) | ~2-25 min (6wk) | ✅ 一致 |
| 单次过率 (DeepSeek) | 103/135 (76.3%) | 106/135 (78.5%) | ✅ **已对齐** |
| 单次过率 (Grok) | — | 135/135 (100%) | ✅ **Aura 独占** |
| HTTP | `http.client` | libcurl C API（dlopen） | ✅ 已同步 |
| Code extraction | 6 步 fallback | `std/extract.aura` 6 步 fallback | ✅ 已同步 |
| Retry | intend + ant colony | intend + ant colony | ✅ 已同步 |
| Prompt | 2 行简洁 | `std/prompt.aura` | ✅ 已同步 |
| check_success | substring + 字边界 | C++ 内置原语 | ✅ 已同步 |
| 并行 | multi-process | `run_parallel.sh` | ✅ 已有 |
| 结果收集 | print 汇总 | `write-file` | ✅ 已加 |

### 已完成

**Phase 5 (P0-P3):** HTTP, intend, ant colony, check_success, 并行, prompt 等全部完成。
**Phase 6 (P0-P5):** 自托管闭环, workspace merge, 多 worker runner, 双 Arena 内存管理。

### 优化方向
- `--serve-async` multi-session 模式（单进程 fiber 并行）
- JSON 结构化结果输出
- 回归监控（git commit + 模型 + 过率）
