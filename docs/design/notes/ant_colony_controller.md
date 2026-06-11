# 蚁群控制器 — 局部变异 × PID 自适应 × LLM 远距离重构

> 把 LLM 当成"蚁后"（产方向），Aura EDSL 作为"工蚁"（大规模局部搜索）。
> CaaS 提供即刻反馈（信息素），PID 控制器指引搜索方向。
> LLM 只在局部搜索耗尽时登场。

---

## 1. 核心洞察

### 1.1 现状的浪费点

当前每一轮 retry 都调用 LLM：

```
LLM (60s, $) ─→ full code ─→ serve.exec → ❌
LLM (60s, $) ─→ tiny fix  ─→ serve.exec → ❌
LLM (60s, $) ─→ tiny fix  ─→ serve.exec → ❌
...
```

微调代码（改一个操作符、换一个边界条件）**不需要 LLM**。
这些可以用 Aura 的 EDSL 原语在本地系统化生成，0 额外 LLM 成本。

### 1.2 可用的本地能力（已实现）

```
set-code "code-string"       → 解析代码到 AST 工作区（节点 ID 稳定）
current-source               → AST → 源码字符串（双向往返）
query:find "name"            → 按名称查找函数/变量节点 ID
query:children node-id       → 获取子节点 ID 列表
query:node node-id           → 获取节点详情（tag/value/sym）
query:calls "fn"             → 找到所有调用某函数的位置
mutate:rebind name new-code  → 按名称替换函数定义
mutate:set-body name body    → 替换函数体
mutate:replace-value id val  → 替换值节点
mutate:replace-type id type  → 替换类型标注
mutate:insert-child pos code → 插入子节点
mutate:remove-node node-id   → 删除节点
eval-current                 → 增量编译+执行工作区 AST
```

**这些全部是本地操作，单次执行 < 1ms。**

### 1.3 技术基础

| 能力 | 当前状态 | 成本 |
|------|---------|:---:|
| AST → Source (round-trip) | `current-source` ✅ | 0 |
| Source → AST (parse) | `set-code` ✅ | < 1ms |
| AST 定点修改 | `mutate:*` 家族 ✅ | < 1ms |
| 增量编译 | `--serve` CaaS ✅ | < 1ms |
| 距离测量 | `measure-distance()` ✅ | < 1ms |
| 相检测 | PID `coarse/fine/putt` ✅ | < 1ms |

---

## 2. 蚁群算法设计

### 2.1 角色映射

| 蚁群 | 本系统 | 对应实现 |
|------|--------|---------|
| **蚁后** (Queen) | **LLM** | 生成新方向/完整重写，成本高，次数少 |
| **工蚁** (Worker) | **EDSL 局部变异** | `mutate:*` + `eval-current` 高速轮询 |
| **信息素** (Pheromone) | **`measure-distance()` 分数** | 变异的输出距离减少量 |
| **食物源** (Food) | **✅ 通过的代码** | `check_success(out, expected)` |
| **蚁穴** (Nest) | **当前 serve 工作区** | 持久编译状态，增量编译 |

### 2.2 搜索策略

```
┌─────────────────────────────────────────────────────────────┐
│                     外循环（LLM 层）                          │
│                                                             │
│  LLM 生成完整方案 → serve.exec() → 成功? → ✅ 结束           │
│                                    ↓  失败                   │
│                     measure-distance() → phase              │
│                                                             │
│                      ┌──── coarse ───→ LLM 重写              │
│                      │                                      │
│                      └─ fine/putt ─→ 内循环                   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                   内循环（EDSL 工蚁层）                        │
│                                                             │
│  当前工作区 AST ← set-code(last_code)                        │
│                                                             │
│  1. AST 扫描 → 发现可修改节点列表                             │
│     ├─ 数值字面量: 42, 0, 1, -1, n (+1), n (-1)             │
│     ├─ 操作符调用: + ↔ -, < ↔ <=, = ↔ not=                  │
│     ├─ 函数调用: (sort lst) ↔ (filter pred lst)              │
│     ├─ 条件分支: if cond t f → if (not cond) f t  (交换)      │
│     ├─ 递归边界: = 0 → <= 0 → = 1 → < 1                     │
│     └─ 输出语句: (display x) → (display (length x)) 等       │
│                                                             │
│  2. 对每个节点生成 N 个局部变体                               │
│     └─ serve 是同一个进程，不走增量编译？                     │
│     答：serve 是同一个进程，用 workspace AST 原地修改           │
│        mutate:rebind → eval-current → 检查                   │
│        失败后不要回退，而是重新 set-code + 下一个变异           │
│                                                             │
│  3. 每个变异被 eval 后，检查输出                              │
│     ✅ → 返回                                                │
│     ❌ → 记录 distance 差值 → 继续下一个变异                    │
│                                                             │
│  4. 如果所有局部变异耗尽 → 回到外循环（LLM）                  │
│     (带当前工作区源码给 LLM 做全面重构)                        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 2.3 变异种类清单

每种变体对应一种"工蚁"类型：

| 工蚁类型 | 操作 | EDSL 实现 | 适用场景 |
|---------|------|----------|---------|
| **数值微调** | 字面量 +1/-1, 翻倍, 取反 | `mutate:replace-value` | 边界条件，数组索引 |
| **操作符交换** | `<`↔`<=`, `+`↔`*`, `=`↔`not=` | `mutate:rebind` | 逻辑错误 |
| **条件翻转** | 交换 if 的 then/else 分支 | `mutate:rebind` | 条件反了 |
| **函数替换** | 同类函数互换 (sort↔filter, map↔for-each) | `mutate:rebind` | 用错了 API |
| **输出改型** | 换 display 参数、加 length、加 join | `mutate:rebind` | 输出格式不对 |
| **递归基改** | 改递归终止条件参数 | `mutate:replace-value` | 无限递归/偏差 |
| **λ 体替换** | 替换 lambda 的 body 部分 | `mutate:set-body` | 映射/过滤函数错 |
| **参数名改** | 替换变量引用 | `mutate:rebind` | 变量名冲突 |
| **类型标注改** | 替换类型 `Int`↔`Float`↔`String` | `mutate:replace-type` | 类型不匹配 |

### 2.4 搜索空间裁剪 — PID 指导

不是所有节点都能改。PID 控制器决定搜索深度：

```scheme
;; lib/std/ant.aura — 蚁群控制核心
(define ant-colony-search
  (lambda (workspace expected phase max-workers)
    (cond
      ;; coarse: 距离太远，局部搜索无效，直接返回 #f 让 LLM 处理
      ((= phase "coarse") #f)
      
      ;; fine: 中等距离，全面搜索
      ((= phase "fine")
       (let ((nodes (scan-mutables workspace)))
         ;; 按 pheromone 分数排序（历史经验）
         (let ((sorted (sort-by-pheromone nodes)))
           ;; 取前 max-workers 个节点试
           (ant-worker-cycle sorted expected max-workers))))
      
      ;; putt: 很近，只试高概率变异
      ((= phase "putt")
       (let ((nodes (scan-mutables workspace)))
         ;; 只取 pheromone > threshold 的节点
         (let ((hot (filter hot-node? nodes)))
           (ant-worker-cycle hot expected 5))))  ;; 最多 5 个
      
      (#t #f))))
```

---

## 3. 核心循环实现

### 3.1 内循环：工蚁搜索

```
function internal_colony_search(target_repo, expected, phase):
    # 1. 获取当前工作区源码
    current_src = serve.exec("(current-source)")
    if !current_src: return false

    # 2. set-code 到工作区
    if !serve.exec(set-code(current_src)): return false

    # 3. 找到可修改的函数
    nodes = serve.exec(query_find_mutables(current_src))

    # 4. 按优先级遍历节点
    for node in prioritized_nodes(nodes, phase):
        # 为这个节点生成所有合理变体
        variants = generate_variants(node, current_src)
        
        for variant in variants:
            # 应用变异
            ok, out = serve.exec(variant)  # 包含 eval-current
            if ok and check_success(out, expected):
                return true, out
            
            # 记录距离变化（信息素更新）
            distance_before = measure_distance(phase)
            distance_after  = measure_distance(rc=0, output=out, expected=expected)
            update_pheromone(node, variant, distance_before - distance_after)
        
        # 恢复工作区（重新 set-code）
        serve.exec(set-code(current_src))

    # 5. 所有工蚁耗尽 → 回到蚁后
    return false
```

### 3.2 外循环：LLM 层

```
function run_with_colony(task, max_rounds):
    serve = new ServeClient(taks)

    for round in 1..max_rounds:
        # 蚁后层：LLM 生成
        llm_code = LLM.generate(task.prompt, task.feedback_history)
        code = extract_code(llm_code)

        # 直接检测（完整代码或 EDSL）
        if code starts with "(set-code":
            ok, out = serve.exec(code + "(eval-current)")
        else:
            ok, out = serve.exec(code)

        if ok and check_success(out, expected):
            return PASS

        # PID 相检测
        phase, ratio, diag = measure_distance(rc, output, expected)

        if phase == "coarse":
            # 太远了，蚁后干活
            feedback = build_coarse_feedback(out, expected)
            add_llm_message(feedback)
            continue

        # fine/putt: 先派工蚁
        found, ant_out = internal_colony_search(serve, expected, phase)
        if found:
            return PASS

        # 工蚁耗尽了 → 蚁后全面重写
        feedback = build_colony_exhausted_feedback(serve, current_src, diag)
        add_llm_message(feedback)

    return FAIL
```

### 3.3 单次 exec 的 CaaS 利用

关键设计：所有内循环变异**在同一个 serve 进程中完成**。
不需要为每个 variant 启动新进程。

```
同一个 serve 进程:
  exec("(set-code \"(define (f x) (+ x 1))\")")  → set workspace
  exec("(mutate:rebind \"f\" \"(lambda (x) (- x 1))\")(eval-current)")  → ✅?
  exec("(set-code \"(define (f x) (+ x 1))\")")  ← 恢复
  exec("(mutate:rebind \"f\" \"(lambda (x) (* x 2))\")(eval-current)")  → ✅?
  exec("(set-code \"(define (f x) (+ x 1))\")")  ← 恢复
  ...
```

每次 mutation 前重新 `set-code` 就恢复了原始状态。
CaaS 增量编译在同一个进程内，< 1ms 一次。

---

## 4. 信息素系统

### 4.1 信息素分数计算

每个变异类型维护一个信息素分数：

```
pheromone[mutation_type] += 
    Δdistance × (1.0 / attempt_count) × decay_factor

其中：
  Δdistance = distance_before - distance_after（正数=改进）
  attempt_count = 这个变异类型被尝试的次数
  decay_factor = 0.95（每轮衰减，避免旧经验过度影响）
```

**信息素高** → 这个变异类型历史上更有效 → 优先尝试。
**信息素低** → 效果不好的变异 → 排在后面。

### 4.2 信息素在 serve 中持久化

```scheme
;; lib/std/ant.aura
(define pheromone-table (hash))

(define add-pheromone!
  (lambda (mutation-type delta)
    (let ((current (or (hash-ref pheromone-table mutation-type) 0.0)))
      (hash-set! pheromone-table mutation-type 
                 (+ current (* 0.95 delta))))))

(define get-pheromone
  (lambda (mutation-type)
    (or (hash-ref pheromone-table mutation-type) 0.0)))

(define decay-all!
  (lambda ()
    ;; 每轮全体衰减，防止信息素爆炸
    (for-each (lambda (k)
                (hash-set! pheromone-table k 
                           (* 0.95 (hash-ref pheromone-table k))))
              (hash-keys pheromone-table))))
```

### 4.3 信息素的 PID 反馈集成

```scheme
(define ant-feedback
  (lambda (name output expectedstdllb phase ratio diag &rest current-src)
    (let ((best-mutation ""))
      ;; 从信息素表选出最高分的变异类型
      (let ((max-pheromone 0.0))
        (for-each (lambda (type)
                    (when (> (get-pheromone type) max-pheromone)
                      (set! max-pheromone (get-pheromone type))
                      (set! best-mutation type)))
                  (list "value-tweak" "op-swap" "cond-flip"
                        "fn-replace" "output-change" "rec-base"
                        "lambda-body" "param-rename" "type-change")))
      
      ;; 信息素最高的变异类型注入提示
      (string-append
        "=== Ant Colony Diagnosis ===\n"
        "Phase: " phase " | Ratio: " (number->string ratio) "\n"
        "Missing: " diag "\n"
        "Best local mutation so far: " best-mutation "\n"
        "Pheromone score: " (number->string max-pheromone) "\n"))))
```

---

## 5. 变异生成器设计

### 5.1 Python 层的变异生成

```python
# 变异生成器 — Python 端，在 LLM 和 serve 之间
# 不需要 C++ 改动，所有 EDSL 原语已存在。

VARIANT_GENERATORS = {
    "value-tweak": lambda node_val: [
        str(int(node_val) + 1),
        str(int(node_val) - 1),
        str(int(node_val) * 2),
        str(int(node_val) // 2),
        "0",
        "1",
    ],
    "op-swap": lambda op: {
        "+": ["-", "*", "/", "max", "min"],
        "-": ["+", "*"],
        "*": ["+"],
        "<": ["<=", ">", "=", "not="],
        "<=": ["<", "="],
        ">": [">=", "<", "="],
        "=": ["not=", "<", ">"],
        "not=": ["="],
    }.get(op, []),
    "cond-flip": lambda: [
        "(if (not cond) else-expr then-expr)"
    ],
    "rec-base": lambda boundary: [
        "(= n 0)", "(<= n 0)", "(= n 1)", "(< n 1)",
        "(= n 2)", "(< n 2)",
    ],
}
```

### 5.2 Aura 层的节点扫描

```scheme
;; 内置于 lib/std/ant.aura
(define scan-mutables
  (lambda ()
    "扫描工作区 AST 返回可修改节点列表"
    (let ((nodes (query:find "")))  ;; 全量节点
      (filter mutable-node? nodes))))

(define mutable-node?
  (lambda (node-id)
    (let ((d (query:node node-id)))
      ;; 可修改节点：含字面量数值的被调用，函数定义的 body，if 条件的子节点
      (let ((tag (car d))
            (val (car (cdr d)))
            (sym (car (cdr (cdr d)))))
        (or (is-value-node? tag val)
            (is-function-def? tag sym)
            (is-if-expr? tag))))))
```

---

## 6. 完整执行流程

```
┌─────────────────────────────────────────────────────────────────┐
│  Attempt 1:                                                      │
│  ─────────────────                                               │
│  LLM → full code (60s) → serve.exec → ❌ (output 30, expect 42) │
│  ↓                                                               │
│  measure-distance → phase = "fine" (ratio = 0.5)                │
│  ↓                                                               │
│  internal_colony_search(serve, expected=[42], phase="fine")      │
│  │                                                               │
│  ├─ 扫描 AST 找到调节点                                           │
│  │  Node 12: literal '5' in body "(+ x 5)"                       │
│  │  Node 18: literal '0' in init-val "(= i 0)"                   │
│  │  Node  7: literal '30' in disp-body                           │
│  │                                                               │
│  ├─ 对 Node 12 (value=5) 生成变体:                               │
│  │  mutate:replace-value 12 6  → eval-current → 31 ❌            │
│  │  mutate:replace-value 12 4  → eval-current → 29 ❌            │
│  │  mutate:replace-value 12 37 → eval-current → 62 ❌            │
│  │  ...                                                          │
│  │                                                               │
│  ├─ 对 Node 18 (value=0) 生成变体:                               │
│  │  mutate:replace-value 18 1  → eval-current → same ❌         │
│  │  mutate:replace-value 18 -1 → eval-current → same ❌         │
│  │                                                               │
│  ├─ 尝试操作符交换:                                              │
│  │  mutate:rebind "f" "(lambda (x) (- x 5))"(eval-current) → 22 ❌ │
│  │  mutate:rebind "f" "(lambda (x) (* x 2))"(eval-current) → 60 ❌ │
│  │  mutate:rebind "f" "(lambda (x) 42)"  (eval-current) → 42 ✅ │
│  │                                                               │
│  └─ ✅ PASS (发现直接返回值 42 即可，无需 LLM)                    │
│                                                                   │
│  总耗时: 60s (LLM) + 20ms (315 次本地变异) ≈ 60s                 │
│  LLM 调用: 1次 (之前需要 3-5次)                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. 成本收益分析

### 7.1 典型场景对比

| 场景 | 当前（贪心） | +蚁群控制器 |
|------|:----------:|:----------:|
| **语义正确，输出格式不对** | LLM 3-5 次 (~180s) | LLM 1 次 + 本地 50ms |
| **边界条件差 1** | LLM 2-3 次 (~120s) | LLM 1 次 + 本地 10ms |
| **操作符用反** | LLM 2-4 次 (~150s) | LLM 1 次 + 本地 5ms |
| **递归条件略偏** | LLM 3-5 次 (~180s) | LLM 1 次 + 本地 20ms |
| **完全写错语法** | LLM 1-2 次 (~60s) | LLM 1-2 次 (coarse) |
| **LLM 方差挂掉** | 5 次 LLM 全错 (300s) | 2 次 LLM + 本地穷举 ✅ |

### 7.2 方差消除效果

```
当前测试（无蚁群）：
  Run 1: table-lookup ❌, contains-duplicate ✅
  Run 2: table-lookup ✅, contains-duplicate ❌
  → 方差 = 1 个随机失败

+蚁群控制器后预期：
  Run 1: table-lookup → 本地变异 5ms 搞定 ✅, contains-duplicate ✅
  Run 2: 同上
  → 方差 ≈ 0（LLM 只做方向引导，本地穷举覆盖微调）
```

### 7.3 成本对比

| 成本项 | 贪心 (5 retry) | 蚁群 (1 LLM + 本地搜索) |
|--------|:-------------:|:----------------------:|
| LLM 调用 | 5 × ~60s = **300s** | 1-2 × ~60s = **60-120s** |
| 本地变异 | 0 | ~300 × 1ms = **300ms** |
| 总耗时 | ~300s | ~61-121s |
| 成本 | **5× $** | **1-2× $** |
| 抗方差 | 低 | **高** |

---

## 8. 实现计划

### Phase 1: Python 端变异器（< 100 行代码）

```python
# 在 run_single_task 中新增 internal_colony_search():
# 1. prompt LLM 生成 N=3 candidates (beam)
# 2. 对每个 candidate, 按 mutate:* 原语系统化变异
# 3. 用同一个 serve 进程 exec + eval-current
# 4. 输出通过对的
# 不修改任何 C++ 代码
```

**需要的 Python 改动：**
- `run_single_task()` 里加 `_internal_colony_search()` 函数
- 使用现有 `serve.exec()` 发送 EDSL 命令
- PID controller 输出 phase=fine/putt 时自动调用

### Phase 2: lib/std/ant.aura（可选，性能优化）

- 把节点扫描逻辑下沉到 Aura stdlib
- 避免 Python 端重复 set-code 的开销
- 信息素表持久化在 serve 中

### Phase 3: 信息素进化的闭环

- 跨任务积累信息素经验
- 高频变异的类型自动优先
- 长期学习不同 LLM（DeepSeek vs MiniMax）的最优变异策略

---

## 9. 架构图

```
┌──────────────────────────────────────────────────────────┐
│                     Python 编排层                          │
│                                                           │
│  run_single_task()                                        │
│    │                                                      │
│    ├── LLM → full_code → serve.exec()                    │
│    │      ↓                                               │
│    ├── measure-distance → phase                           │
│    │      ↓                                               │
│    ├── coarse ──────────────→ LLM 重写                    │
│    │      ↓                                               │
│    └── fine/putt ──→ internal_colony_search()             │
│                          │                                │
│                          ├── scan_mutables(code)          │
│                          ├── generate_variants(node)      │
│                          ├── batch_test(serve, variants)  │
│                          │   ├── same serve process       │
│                          │   ├── set-code + mutate + eval  │
│                          │   ├── eval-current             │
│                          │   └── check_success            │
│                          │      ↓                         │
│                          ├── ✅ found? → return           │
│                          └── ❌ exhausted → LLM fallback  │
│                                                           │
└──────────────────────────┬───────────────────────────────┘
                           │
                    ┌──────┴──────┐
                    │  ./aura --serve  │
                    │  (CaaS)     │
                    │             │
                    │ set-code → AST workspace              │
                    │ query:* → 节点发现                     │
                    │ mutate:* → 定点修改 （同一 workspace） │
                    │ eval-current → 增量编译 → 输出        │
                    │ current-source → AST→源码              │
                    └───────────────────────────────────┘
```

---

## 10. 与现有 PID 控制器的集成

```python
# 现有代码改动最小：在 run_single_task 的 retry 循环中
# 在 ada_fb 构建前插入 colony search

for attempt in range(max_att):
    llm_resp → code → serve.exec(code)
    
    if not success:
        phase, ratio, diag = call_adaptive(rc, output, expected)
        
        if phase in ("fine", "putt"):
            # === 新增：蚁群搜索 ===
            found, ant_out = internal_colony_search(serve, code, expected, phase)
            if found:
                return PASS
            # === 结束新增 ===
        
        # 原来逻辑不变
        ada_fb = build_adaptive_feedback(...)
        correction = build_correction(fb, phase)
        messages.append(correction)
```

---

## 11. 总结

| 概念 | 蚁群类比 | 实现 | 成本 |
|------|---------|------|:---:|
| 蚁后 | LLM | `llm_complete()` | 60s, 高 |
| 工蚁 | 局部变异 | `mutate:*` + `eval-current` | <1ms, 极低 |
| 信息素 | 距离反馈 | `measure-distance()` | <1ms |
| 蚁穴 | serve 进程 | `--serve` + workspace AST | 0 |
| 食物 | 通过的代码 | `check_success()` | 0 |

**核心原则：能用本地计算解决的问题，绝不请求 LLM。**
CaaS 让每次本地变异只需 < 1ms。这是把它当"本地计算"来用的基础。
