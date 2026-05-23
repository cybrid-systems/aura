# E4: 可演化策略 — 设计文档

> 策略不是代码，是数据。数据可以被分析、调整、演化。
>
> 让 `intend` 自己学会怎么解决一类问题。

**更新：2026-05-23**
**状态：Phases 1-3 已完成并集成到 benchmark，Phase 4 远期计划**

---

## 现状

```scheme
;; E2 的 define-strategy 只是一个记录构造器
(define-strategy my-strategy
  generator: gen-fn
  verifier: ver-fn
  fixer: fix-fn
  max-attempts: 5)

;; 注册后 intend 使用
(register-strategy! "my-strategy" my-strategy)
(intend goal strategy: "my-strategy")
```

**问题：**
- 策略定义后就固定了，LLM 不能改它
- 不知道哪些 prompt 有效、哪些任务总是卡住
- 没有跨 runs 的学习机制
- `intend-history` 只记录 timeline，不做聚合分析

---

## E4 设计目标

1. **策略可检视** — 运行时查看策略的 generator/fixer 代码、参数、历史表现
2. **策略可调整** — LLM 通过 `mutate:rebind` 修改策略字段
3. **策略可从历史学习** — 聚合 `intend-history` 数据，自动调优参数
4. **多意图协作** — `(intend A)` 内部可以调 `(intend B)`，形成意图树

---

## 1. 策略作为数据

### 策略记录结构

```scheme
;; 内部表示（C++ 或 Aura record）
(strategy
  name: "adaptive"
  generator: <closure>          ;; (goal) → code
  verifier: <closure>           ;; (code) → "#t" | error-msg
  fixer: <closure>              ;; (code err goal) → new-code
  max-attempts: 5
  temperature: 0.3
  sys-prompt-template: "You are Aura. Return only code."
  created: 1779249000
  evolution: 3                  ;; 第几代
  parent: "generate-and-fix")   ;; 从哪个策略演化而来
```

### 字段访问原语

```scheme
(strategy-field "adaptive" 'generator)      ;; → <closure>
(strategy-field "adaptive" 'max-attempts)   ;; → 5
(strategy-set-field! "adaptive" 'max-attempts 8)
(strategy-set-field! "adaptive" 'generator new-gen-fn)
```

#### 安全限制

`strategy-set-field!` 加白名单 + 类型检查，防止 LLM 误操作：

| 字段 | 可写 | 类型检查 |
|------|------|---------|
| `max-attempts` | ✅ | `(check val : Int)` 且 ≥ 1 ≤ 20 |
| `temperature` | ✅ | `(check val : Float)` 且 0.0 ≤ val ≤ 1.0 |
| `sys-prompt-template` | ✅ | `(check val : String)` |
| `generator` | ✅ | `(check val : closure)` |
| `fixer` | ✅ | `(check val : closure)` |
| `name` | ❌ | 只读 |
| `created` | ❌ | 只读 |
| `evolution` | ❌ | 只由 `evolve-strategy` 写 |
| `parent` | ❌ | 只读 |

```scheme
;; 安全写入（带类型和范围检查）
(strategy-set-field! "adaptive" 'temperature 2.0)
;; → error: temperature must be in range [0.0, 1.0]

(strategy-set-field! "adaptive" 'name "new-name")
;; → error: name is read-only
```

#### strategy-inspect: 一键检视

```scheme
(strategy-inspect "adaptive")
;; → #(strategy-inspect
;;     name: "adaptive"
;;     evolution: 3
;;     parent: "generate-and-fix"
;;     fields: (
;;       ("max-attempts" current: 5 range: "[1,20]" writable: #t)
;;       ("temperature" current: 0.3 range: "[0.0,1.0]" writable: #t)
;;       ("sys-prompt-template" current: "You are Aura..." writable: #t)
;;       ("name" writable: #f)
;;       ("evolution" writable: #f)))
```

返回所有可调字段 + 当前值 + 推荐范围 + 可写性。LLM 调用此函数后决策要改什么。

---

## 2. `intend-analytics` — 历史分析与聚合

新增内置原语，分析 `intend-history` 的聚合数据：

```scheme
(intend-analytics)                   ;; → 所有策略的分析
(intend-analytics "adaptive")        ;; → 指定策略
(intend-analytics "adaptive" "prime-*")  ;; → 按任务名过滤
```

返回结构：

```scheme
#(analytics
  total-runs: 50
  success-rate: 0.76                ;; 76% 成功率
  avg-attempts: 2.1                 ;; 平均尝试次数
  median-attempts: 2
  total-llm-calls: 157
  total-llm-tokens: 284000
  top-failing-tasks: (              ;; 最常失败的任务
    #(task: "merge-sort" fail-rate: 0.4 avg-attempts: 3.8)
    #(task: "hash-stats" fail-rate: 0.3 avg-attempts: 3.2))
  top-fix-patterns: (               ;; 常见修复模式
    #(error: "unbound variable" count: 23)
    #(error: "type mismatch" count: 12))
  by-task: (                        ;; 每个任务的详细数据
    #(task: "arith-basic" runs: 5 success: 5 avg-attempts: 1.0)
    #(task: "prime-test" runs: 5 success: 4 avg-attempts: 2.4))))
```

### 历史记录 Schema

每条 `intend` 运行时记录留一条结构化记录：

| 字段 | 类型 | 说明 |
|------|------|------|
| `record-id` | uint64 | 自增 ID，全局唯一 |
| `strategy-name` | string | 使用的策略名 |
| `task-desc` | string | goal 原文（或从 `;; task:` 提取的任务名） |
| `params` | hash | 调用时的参数快照：temperature、max-attempts 等 |
| `success?` | bool | 最终是否成功 |
| `attempts` | int | 实际尝试次数 |
| `errors` | string[] | 每次失败的错误消息 |
| `error-types` | string[] | 按错误模式分类后的类型标签 |
| `generated-codes` | string[] | 每次 LLM 生成的代码 |
| `llm-call-count` | int | LLM 调用次数（含修正） |
| `llm-tokens` | int | 估计 token 消耗 |
| `duration-ms` | int | 该次 intend 总耗时 |
| `timestamp` | int64 | Unix 时间戳 |
| `parent-record-id` | uint64 | 如果是嵌套 intend 的子意图，指向父记录 |

```cpp
struct IntendRecord {
    uint64_t record_id;
    std::string strategy_name;
    std::string task_desc;        // goal 原文或 ;; task: 标记
    bool success;
    int attempts;
    std::vector<std::string> errors;
    std::vector<std::string> error_types;  // 分类后的错误类型标签
    std::vector<std::string> generated_codes;
    uint64_t llm_call_count;
    uint64_t llm_tokens;          // 估计值
    uint64_t duration_ms;
    uint64_t timestamp;
    uint64_t parent_record_id;    // 0 = root
};
std::vector<IntendRecord> intend_history_;
```

### 存储策略：滑动窗口

为防止内存膨胀，默认只保留最近 N 条记录：

```cpp
static constexpr size_t MAX_HISTORY_SIZE = 1000;
// 插入时若超出，删除最老的
if (intend_history_.size() >= MAX_HISTORY_SIZE)
    intend_history_.erase(intend_history_.begin());
intend_history_.push_back(record);
```

N 可通过 `(intend-config 'history-limit 5000)` 调整。超出限制时自动裁剪旧记录。

### 错误类型分类

`errors` 字段保存原始错误消息，`error-types` 保存分类后的标签，方便 analytics 聚合：

| 原始错误示例 | 分类标签 |
|-------------|---------|
| "unbound variable: x" | "unbound-variable" |
| "type mismatch: expected Int got String" | "type-mismatch" |
| "division by zero" | "div-zero" |
| "recursion limit exceeded" | "recursion-limit" |
| "syntax error" | "syntax-error" |
| "timeout" | "timeout" |
| （其他无法匹配的 C++/Aura 错误） | "other" |

分类逻辑在 C++ `intend` 循环中：字符串模式匹配 → 标签（编译期做，无 LLM 调用）。

### `intend-analytics` 聚合逻辑

```
intend-analytics(strategy?, task-pattern?):
  1. 从 intend_history_ 过滤匹配的记录
  2. 按 strategy 分组
  3. 按 task_desc 分组（支持通配符 *）
  4. 计算各组的:
     - success-rate = successes / total
     - avg-attempts = total attempts / total
     - top errors = 按 error-type 频率排序
     - avg-llm-calls, avg-duration
  5. 返回聚合结构
```

支持 `:filter` 精确过滤：
```scheme
(intend-analytics "adaptive" :filter error-type "type-mismatch")
;; → 只返回 type-mismatch 错误相关的历史实现
```

---

## 3. `evolve-strategy` — 自动调优

```scheme
;; 从分析数据自动调整策略参数
(evolve-strategy "adaptive"
  (intend-analytics "adaptive"))

;; 返回新策略（不自动注册）
;; → strategy (自动调整了 temperature / max-attempts / sys-prompt)

;; 也可以手动指定要调什么
(evolve-strategy "adaptive"
  (intend-analytics "adaptive")
  fields: '(max-attempts temperature sys-prompt-template))
```

### 调优启发式

| 指标 | 调整 |
|------|------|
| success-rate < 50% 且 avg-attempts = max-attempts | ↑ max-attempts (加 2) |
| success-rate > 90% 且 avg-attempts = 1 | ↓ max-attempts (降到 3) |
| 高频错误 "unbound-variable" | sys-prompt 追加 "Do NOT use undefined variables" |
| 高频错误 "type-mismatch" | sys-prompt 追加 "Use (check x : Type) before operations" |
| 高频错误 "div-zero" | sys-prompt 追加 "Guard division with (if (= d 0) ...)" |
| 高频错误 "syntax-error" | sys-prompt 追加 "Check parentheses carefully" |
| avg LLM response 含大量解释性文字 | sys-prompt 追加 "Return ONLY code, NO explanation" |
| temperature 过高导致方差大（stddev > 0.3） | ↓ temperature (降 0.1) |
| temperature 过低导致重复同一种失败 | ↑ temperature (升 0.1, 只要 ≤1.0) |

### 安全机制：保留旧版本 + 探索/利用平衡

```scheme
;; evolve-strategy 返回新策略（保留旧版本，不覆盖）
(evolve-strategy "adaptive"
  (intend-analytics "adaptive")
  reason: "success-rate dropped below 50%, increased max-attempts from 5 to 7")

;; 注册新版本
(register-strategy! "adaptive-v2" evolved-strategy)

;; 旧版本仍然可用
(get-strategy "adaptive")    ;; → 原始版本
(get-strategy "adaptive-v2") ;; → 演化版本
```

**探索/利用平衡：** 每次 evolve 结果以概率 ϵ 保留原始值（否则采用新值），避免因单次异常数据过度调优：

```scheme
;; epsilon = 0.1 （10% 概率保持原值）
(if (< (random) 0.1)
    old-temp    ;; 探索：保留原值
    new-temp)   ;; 利用：使用新值
```

### 实现方式

纯 Aura stdlib 函数，内部调用 `mutate:rebind` 做结构级修改：

```scheme
(define (evolve-strategy name analytics . kwargs)
  (let* ((old (get-strategy name))
         (reason (keyword-ref kwargs 'reason
                   (string-append "evolved from " name)))
         (fields (keyword-ref kwargs 'fields
                   '(max-attempts temperature sys-prompt-template)))
         (rate (analytics-field analytics 'success-rate))
         (avg-att (analytics-field analytics 'avg-attempts))
         (max-att (strategy-field old 'max-attempts))
         (temp (strategy-field old 'temperature))
         (sp (strategy-field old 'sys-prompt-template))
         (errors (analytics-field analytics 'top-error-types))
         (epsilon 0.1))
    ;; 调 max-attempts
    (if (and (< rate 0.5) (>= avg-att max-att))
        (set! max-att (+ max-att 2))
        (if (and (>= rate 0.9) (<= avg-att 1))
            (let ((new-max (max 3 (- max-att 2))))
              (if (< (random) epsilon) (set! new-max max-att))
              (set! max-att new-max))))
    ;; 调 temperature
    (let ((delta (if (> rate 0.8) -0.05 0.05)))
      (let ((new-temp (+ temp delta)))
        (if (< (random) epsilon) (set! new-temp temp))
        (set! temp (max 0.0 (min 1.0 new-temp)))))
    ;; 从错误模式调 sys-prompt
    (for-each (lambda (err-type)
      (cond ((= err-type "unbound-variable")
             (set! sp (string-append sp
               "\nCRITICAL: Do NOT use undefined variables. Always (define ...) first.")))
            ((= err-type "type-mismatch")
             (set! sp (string-append sp
               "\nCRITICAL: Use (check x : Type) before operations.")))
            ((= err-type "syntax-error")
             (set! sp (string-append sp
               "\nCRITICAL: Check parentheses. Every (if COND THEN ELSE) needs 3 sub-forms.")))))
      (analytics-field analytics 'top-error-types))
    ;; 构造新策略
    (let ((new (strategy-copy old)))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'max-attempts max-att))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'temperature temp))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'sys-prompt-template sp))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'evolution
          (+ (strategy-field old 'evolution) 1)))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'parent name))
      (mutate:rebind "strategy-field"
        (strategy-set-field! new 'evolve-reason reason))
      new)))
```

---

## 4. 多意图协作

### 嵌套 intend

```scheme
;; 策略内部可以调用子 intend
(intend "Write a merge sort"
  strategy: "adaptive"
  on-fix: (lambda (code err goal)
    ;; 如果修复失败，尝试用子 intend 分析错误
    (let ((analysis (intend
                      (string-append
                        "What is the error in this code: " code
                        " Error: " err)
                      strategy: "analyze-error")))
      ;; 用分析结果指导修复
      (intend (string-append goal
                " Previous attempt failed with: " analysis)
              strategy: "generate-and-fix"))))
```

### 意图树可视化

```scheme
(intend-tree) → 返回当前活跃的意图调用树

;; 输出格式
#(intend-tree
  roots: ((
    #(node
      id: 1
      goal: "Write merge sort"
      strategy: "adaptive"
      status: "running"
      children: (
        #(node id: 2 goal: "Analyze error" strategy: "analyze-error" status: "done")
        #(node id: 3 goal: "Regenerate with analysis" strategy: "generate-and-fix" status: "running"))))))
```

---

## 5. 集成与测试

### benchmark 自进化测试

Phase 3 完成后在 `edsl_benchmark.py` 加 `--evolve` 模式：

```bash
# 跑 5 轮 intend（不进化）
python3 tests/edsl_benchmark.py --rounds 5 --intend --output baseline.json

# 跑 5 轮 intend（每轮后进化）
python3 tests/edsl_benchmark.py --rounds 5 --intend --evolve --output evolved.json

# 对比
python3 tests/edsl_benchmark.py --compare baseline.json evolved.json
# → 

最终的完整闭环：

```
┌─────────────────────────────────────────────────────────┐
│  1. (intend "Write merge sort")                          │
│     → 调用 "adaptive" 策略                                │
│     → 失败 3 次, 最终成功                                │
│     → 记录到 intend_history_                              │
│                                                          │
│  2. (intend-analytics "adaptive")                        │
│     → 发现 success-rate 下降, avg-attempts 上升           │
│                                                          │
│  3. (evolve-strategy "adaptive" ...)                     │
│     → 调高 temperature, 追加 system prompt 规则           │
│     → 注册为 "adaptive-v2"                                │
│                                                          │
│  4. (intend "Write merge sort" strategy: "adaptive-v2")  │
│     → 第一次就成功                                        │
│                                                          │
│  5. LLM 主动触发的演化:                                   │
│     (mutate:rebind "strategy-field"                      │
│       (strategy-set-field! "adaptive-v2"                  │
│         'generator new-gen-fn))                          │
└─────────────────────────────────────────────────────────┘
```

---

## 6. 实现计划

### Phase 1: 结构化 intend history（~2h）
- `timeline_` 从 `vector<string>` 改为 `vector<IntendRecord>`
- 记录 strategy name、task name、attempts、errors
- `intend-analytics` 原语 — 按策略/任务聚合统计

### Phase 2: 策略字段访问（~1h）
- `strategy-field` / `strategy-set-field!` 原语
- 允许 `mutate:rebind` 修改策略字段
- 测试：LLM 动态改 temperature

### Phase 3: evolve-strategy（~2h）
- 用 Aura stdlib 实现 `evolve-strategy` 函数
- 实现上述调优启发式
- 集成到 benchmark 作为 `--evolve` 模式

### Phase 4: 多意图树（远期）
- `intend-tree` 原语
- 嵌套 intend 调用支持
- 意图树可视化


### 反馈回路：LLM 看到自己的进化历史

`intend-analytics` 的结果可以直接注入 LLM 的 system prompt：

```scheme
;; 每次 intend 前，自动追加历史分析
(define (intend-with-history goal strategy)
  (let ((history (intend-analytics strategy)))
    (intend (string-append goal
              "\n\nYour recent performance: "
              (analytics-summary history))
            strategy: strategy)))
```

这让 LLM 看到："你上次的 merge-sort 失败了 3 次，common error 是 type-mismatch"。

LLM 可以在下一次生成时有意识避免同样的错误模式，与 `evolve-strategy` 形成互补——一个是 LLM 自觉调整行为，一个是系统自动调参数。

---

## 6. 安全与限制

| 风险 | 缓解 |
|------|------|
| LLM 滥用 strategy-set-field! | 白名单 + 类型检查 + 范围检查 |
| evolve-strategy 过度自信 | epsilon-greedy 探索、保留旧版本 |
| 历史数据膨胀 | 滑动窗口（默认 1000 条） |
| 单次异常数据触发误调优 | 只在连续 3+ 次演化后生效 |
| 嵌套 intend 死循环 | max-depth 限制（默认 5） |

---

## 7. LLM 自演化循环（完整）

最终的完整闭环：
---

## 开放问题

1. **如何自动识别 task name？** 从 goal 字符串推导（"Write merge sort" → "merge-sort"），或用 `;; task:` 标记
2. **evolve-strategy 的收敛条件？** 当连续 3 次演化提升 < 5% 时停止
3. **多意图的并发？** 初期简单顺序执行，以后加 parallel strategy
4. **history 持久化？** 当前在内存中，重启丢失。E5 加文件/数据库后端
