# E4: 可演化策略 — 设计文档

> 策略不是代码，是数据。数据可以被分析、调整、演化。
>
> 让 `intend` 自己学会怎么解决一类问题。

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

这允许 LLM 在 EDSL 会话中通过 `mutate:rebind` 动态调整：

```scheme
;; LLM 生成的动作序列
(mutate:rebind "strategy-field"
  (strategy-set-field! "adaptive" 'temperature 0.5))
```

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

### 实现思路

数据结构：在 `evaluator_impl.cpp` 中扩展 `timeline_` 为结构化记录列表，不只是一个字符串列表。

每次 `intend` 完成后追加一条记录：

```cpp
struct IntendRecord {
    std::string strategy_name;
    std::string goal;
    uint64_t timestamp;
    bool success;
    int attempts;
    std::vector<std::string> errors;  // 每次失败的错误
    uint64_t llm_call_count;
};
std::vector<IntendRecord> intend_history_;
```

`intend-analytics` 遍历 `intend_history_` 做聚合：

- 按 strategy name 分组
- 按 task name（从 goal 推导或显式标记）分组
- 统计 success rate、avg attempts、common errors

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
| 高频错误 "unbound variable" | sys-prompt 追加 "Do NOT use undefined variables" |
| 高频错误 "type mismatch" | sys-prompt 追加 "Use (check x : Type) before operations" |
| avg LLM response 含大量解释性文字 | sys-prompt 追加 "Return ONLY code, NO explanation" |
| temperature 过高导致方差大 | ↓ temperature (降 0.1) |
| temperature 过低导致重复失败 | ↑ temperature (升 0.1) |

### 实现方式

纯 Aura stdlib 函数（不需要 C++ 原语）：

```scheme
(define (evolve-strategy name analytics)
  (let ((old (get-strategy name))
        (rate (analytics-field analytics 'success-rate))
        (avg-att (analytics-field analytics 'avg-attempts))
        (max-att (strategy-field old 'max-attempts))
        (temp (strategy-field old 'temperature))
        (sp (strategy-field old 'sys-prompt-template)))
    ;; 调 max-attempts
    (if (and (< rate 0.5) (>= avg-att max-att))
        (set! max-att (+ max-att 2))
        (if (and (>= rate 0.9) (<= avg-att 1))
            (set! max-att (max 3 (- max-att 2)))))
    ;; 调 temperature
    (if (> rate 0.8)
        (set! temp (- temp 0.05))    ;; 成功率高 → 降方差
        (set! temp (+ temp 0.05)))   ;; 成功率低 → 升探索
    ;; 构造新策略
    (let ((new (strategy-copy old)))
      (strategy-set-field! new 'max-attempts max-att)
      (strategy-set-field! new 'temperature temp)
      (strategy-set-field! new 'evolution (+ (strategy-field old 'evolution) 1))
      (strategy-set-field! new 'parent name)
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

## 5. LLM 自演化循环

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

---

## 开放问题

1. **如何自动识别 task name？** 从 goal 字符串推导（"Write merge sort" → "merge-sort"），或用 `;; task:` 标记
2. **evolve-strategy 的收敛条件？** 当连续 3 次演化提升 < 5% 时停止
3. **多意图的并发？** 初期简单顺序执行，以后加 parallel strategy
4. **history 持久化？** 当前在内存中，重启丢失。E5 加文件/数据库后端
