# 蚁群控制器 — 解耦架构与实现计划

> 当前实现分析 + 重构方案。
> 把 Python 编排层、Aura 控制层、EDSL 变异层彻底解耦。

---

## 1. 现状问题

### 1.1 三层耦合

```
Python (run_single_task)
  ├── LLM 调用
  ├── 代码提取 (extract_code)
  ├── phase 检测 (call_adaptive → Aura 子进程)
  ├── 殖民地搜索 (Python 字符串级别)
  │     ├── _find_mutables (re.finditer)
  │     ├── _gen_variants (字符串替换)
  │     └── serve.exec(variant_code)     ← 每次全量编译
  ├── 自适应反馈 (build_adaptive_feedback)
  └── LLM 重试
```

**问题：**
1. `internal_colony_search` 在 Python 里做**字符串操作**，不用 Aura 的 `query:*` / `mutate:*` / `current-source`
2. 每个变体 = 全量编译+执行，不是 EDSL 增量编译
3. PID 相位检测 (Aura) 和殖民地搜索 (Python) 跨进程通信，每次都 `subprocess.run`
4. Scheme 兼容定义在 `run_code` 的 `code.replace()` 里，和 `query:find` 不互通
5. 信息素不存在

### 1.2 性能数据验证

```
当前 (Python 字符串):
  20 个变体: ~400-800ms  (每个全量编译 ~20-40ms)
  
设计目标 (EDSL mutate):
  20 个变体: ~20ms       (每个 EDSL 原地修正 <1ms)
  
信息素 + PID 裁剪:
  全量扫描:     20 变体
  PID 裁剪后:   3-5 变体 (hot nodes only)
  总时间:       ~3-5ms
```

---

## 2. 目标架构

### 2.1 三层分离

```
Layer 3: Python 编排层 (薄层)
───────  ──────────────────────────────────
  run_single_task()     ← 57次调用，每个任务独立
    │
    ├── LLM.call()      ← 唯一的 LLM 交互点
    ├── serve.exec()    ← 唯一的 EDSL 交互点
    └── serve.exec("(colony:search ...)")  ← 一次调用，整批搜索
    │
    └── 结果收集 + 统计

Layer 2: Aura 控制层 (lib/std/ant.aura)
───────  ──────────────────────────────────
  measure-distance()    ← 已有 (adaptive.aura)
  colony:search()       ← 核心搜索循环 (新建)
  colony:feedback()     ← 结构化反馈
  colony:pheromone-*    ← 信息素系统

Layer 1: Aura EDSL 原语 (C++)
───────  ──────────────────────────────────
  query:find / query:children / query:node
  current-source / set-code
  mutate:rebind / mutate:replace-value / mutate:set-body
  eval-current
```

### 2.2 调用时序

```
Python                            Aura Serve
──────                            ──────────
LLM → full code
  │
  ├─────────────────────────────→ serve.exec(code)
  │              ok/out/err ←───
  │
  ├─────────────────────────────→ serve.exec("(measure-distance ...)")
  │              phase/ratio ←──
  │
  ├──── phase = "fine/putt"? ───
  │
  ├─────────────────────────────→ serve.exec("""(require "std/ant" all:)
  │                                         (colony:search phase
  │                                          expected-list
  │                                          max-variants
  │                                          timeout-ms)""")
  │              pass/out ←───── or fail/out
  │
  ├── pass? → return
  │
  └── LLM retry → 回到顶部
```

### 2.3 关键变化：一次 Aura 调用代替 20 次

```
当前:
  Python:  for variant in 20 variants:
              ok, out = serve.exec(variant)   ← 20 次 IPC + 20 次全量编译

目标:
  Python:  ok, out = serve.exec("""(colony:search phase expected-list)""")
                                        ← 1 次 IPC, 0 次全量编译
                                          Aura 层内部做 mutate + eval-current
```

---

## 3. 实现计划

### Phase A: EDSL 化殖民地搜索 (0 C++ 修改)

**目标：** 把 Python 字符串变异替换为 EDSL 命令，每次变体从 80ms → <1ms。

**改动范围：** `tests/edsl_benchmark.py` 内的 `_find_mutables` + `_gen_variants`

```python
# 当前 (字符串替换):
def _gen_variants(code, mutables, expected):
    for item in mutables:
        if item[0] == "literal":
            new_code = code[:start] + str(new_val) + code[end:]
            yield new_code, f"lit {val}->{new_val}"
            # 每次 yield = 全量编译

# 目标 (EDSL 命令):
def _gen_edsl_mutations(code, mutables, expected):
    """不直接生成变体代码，生成 EDSL 命令列表"""
    esc = _ada_esc(code)
    mutations = []
    for item in mutables:
        if item[0] == "literal" and item[1] == "node-id":
            for delta in [1, -1, 2, -2]:
                cmds = [
                    f'(set-code "{esc}")',
                    f'(mutate:replace-value {item[1]} {item[1] + delta})',
                    '(eval-current)',
                ]
                mutations.append((';'.join(cmd for cmd in cmds), f"lit {delta}"))
    return mutations
```

但这有前置条件：需要知道节点 ID。目前 `_find_mutables` 是从文字分析找数值，不知道节点 ID。需要增加一步：

```python
# 先在 serve 里 set-code + query 获取节点信息
serve.exec(f'(set-code "{esc}")')
nodes = serve.exec('(query:find "display")')
# 然后用节点 ID 做变异
serve.exec(f'(set-code "{esc}")(mutate:replace-value {node_id} {new_val})(eval-current)')
```

这样每个变体 1 次 IPC（set-code + mutate + eval-current 组合在一次 exec）。从 20 次全量编译降到 20 次 EDSL 增量编译。

**工作量：** ~50 行 Python 改动。

---

### Phase B: 信息素系统 (0 C++ 修改)

**目标：** 在 Aura 中维护信息素表，Python 按信息素排序变异顺序。

```scheme
;; lib/std/ant.aura — 信息素系统

;; 信息素表在 serve 进程内存中持久化
(define pheromone-table (hash))

(define (pheromone:init)
  (set! pheromone-table (hash))
  ... 初始化默认信息素值)

(define (pheromone:update mutation-type delta-distance)
  ;; delta-distance > 0 = 这次变异有效
  ;; 信息素 = 历史衰减 + 新收益
  (let ((current (or (hash-ref pheromone-table mutation-type) 0.0)))
    (hash-set! pheromone-table mutation-type
               (+ (* current 0.95) delta-distance))))

(define (pheromone:rank mutation-types)
  ;; 按信息素从高到低排序变异类型
  (sort mutation-types
        (lambda (a b)
          (> (or (hash-ref pheromone-table a) 0.0)
             (or (hash-ref pheromone-table b) 0.0)))))
```

Python 端：

```python
def _rank_mutation_types(serve):
    ranking = serve.exec('(pheromone:rank (list "literal" "op-swap" "display" "fn-call"))')
    return parse_list(ranking)
```

**工作量：** ~30 行 Aura + ~20 行 Python。

---

### Phase C: 最少 IPC 殖民地搜索 (实用版本)

**设计限制：** Aura 的 `eval-current` 将 display 输出直接写到 stdout，
无法从 Aura 函数内部捕获。纯 Aura 搜索循环不可行。

**替代方案：** 保持 Python 编排层，但把整个搜索循环封装为
一个 serve.exec 调用。serve 端返回全部结果。

```python
# 当前: 每变体 1 次 serve.exec = 20 次 IPC
for variant in variants:
    ok, out = serve.exec(variant)

# Phase C: 1 次 serve.exec, 服务端做批量测试
# 但受限于 eval-current 输出捕获, 实际等效于 Phase A
```

**实际收益：** Phase A 已经做到每变体 <1ms（EDSL 增量编译），
IPC 开销 (20ms) 成为瓶颈。Phase C 目标：1 次 IPC 完成搜索。
需要 Aura 新增 `eval-current-return-output` 原语，
或修改 `display` 行为支持输出捕获。

**目标：** 把搜索循环从 Python 移到 Aura，一次 serve.exec 完成。

```scheme
;; lib/std/ant.aura — 殖民地搜索
(export colony:search)

;; (colony:search phase expected-list [max-variants]) -> list (found? output)
(define (colony:search phase expected-str max-variants)
  (if (= phase "coarse") (list #f "")
    (let ((node-list (scan-mutables)))         ;; 扫描工作区 AST
      (let ((mutations (generate-mutations node-list)))  ;; 生成变体
        (if (null? mutations) (list #f "")
          (let search ((remaining mutations)
                       (count 0)
                       (last-output ""))
            (if (or (null? remaining) (> count max-variants))
                (list #f last-output)
              (let ((m (car remaining)))
                (apply-mutation m)             ;; mutate:* + eval-current
                (let ((result (eval-current)))
                  (if (check-output result expected-str)
                      (list #t result)          ;; ✅ 找到了
                    (let ((delta (measure-distance 0 result expected-str)))
                      (pheromone:update (car m) delta)  ;; 更新信息素
                      (undo-mutation m)                 ;; set-code 恢复
                      (search (cdr remaining)
                              (+ count 1)
                              result))))))))))))
```

需要新增的原语：
- `scan-mutables` → 扫描工作区 AST 返回可变异节点列表
- `generate-mutations` → 基于节点列表生成变体
- `apply-mutation` → 执行 EDSL mutate + eval-current
- `undo-mutation` → set-code 恢复原始状态
- `check-output` → 关键词匹配

**工作量：** ~120 行 Aura。可以在 `lib/std/ant.aura` 中通过调用现有 `query:*` 和 `mutate:*` 实现，不需要 C++ 改动。

---

### Phase D: PID 裁剪 + 信息素利用 (高级)

**目标：** 用量度分数动态控制搜索深度。

```scheme
(define (colony:search-with-pid phase ratio expected max-variants)
  (let ((hot-nodes (cond
                     ((= phase "putt")
                      ;; 只搜信息素 > 阈值的节点, 最多 5 个
                      (take (pheromone:hot-nodes) 5))
                     ((= phase "fine")
                      ;; 搜所有可变节点, 按信息素排序
                      (pheromone:rank (scan-mutables)))
                     (#t '()))))  ;; coarse = 不搜
    ...))
```

信息素的跨任务持久化：
```scheme
;; 任务结束时，把信息素表序列化
(define (pheromone:export)
  (json-encode (hash->alist pheromone-table)))

;; 任务开始时，导入历史信息素
(define (pheromone:import json-str)
  (set! pheromone-table (alist->hash (json-parse json-str))))
```

Python 端：
```python
# 任务结束
pheromone = serve.exec('(pheromone:export)')
save_to_file(f"memory/pheromone-{model}.json", pheromone)

# 下个任务开始
pheromone = load_from_file(f"memory/pheromone-{model}.json")
serve.exec(f'(pheromone:import "{pheromone}")')
```

**工作量：** ~60 行 Aura + ~20 行 Python。

---

## 4. 收益预期

| 指标 | 当前 (Python 字符串) | Phase A (EDSL 化) | Phase C (纯 Aura) |
|------|:-----------------:|:---------------:|:---------------:|
| 每变体成本 | ~20-40ms | **~1-3ms** | **<1ms** |
| IPC 次数/搜索 | 20 | 20 | **1** |
| 搜索上限 | 20 var / 8s | 200 var / 8s | **1000 var / 8s** |
| 代码安全性 | 低 (字符串断裂) | 高 (AST 保证) | 高 |
| C++ 修改 | 0 | 0 | 0 |
| 信息素 | 0 | 0 | 有 |
| 跨任务学习 | 0 | 0 | 有 |

### 4.1 覆盖范围扩展

Phase C 的 1000 变体/次搜索可以覆盖：
- **数值微调**：10 个数值 × 10 种变体 = 100
- **操作符交换**：15 个操作符 × 3 种变体 = 45
- **函数替换**：12 个函数 × 4 个别名 = 48
- **条件分支交换**：5 个 if × 2 种 = 10
- **递归基修改**：5 个边界 × 5 种 = 25
- **display 格式**：3 个 display × 6 种格式 = 18
- **函数体替换**：3 个 λ × 4 种 body = 12

总计：~260 个变体 / 搜索，远超当前 20 个限制。

---

## 5. 时间线

| Phase | 工作量 | 依赖 | 预期完成 |
|-------|:-----:|------|:--------:|
| A: EDSL 化变异 | ~50 行 Python | 无 | **~1h** |
| B: 信息素系统 | ~50 行 Aura+Python | A | **~1h** |
| C: 纯 Aura 搜索 | ~120 行 Aura | A+B | **~2h** |
| D: PID 裁剪 | ~80 行 Aura+Python | C | **~1h** |

总计：~5h（全是 Aura/Python，不需要改 C++）。

---

## 6. 架构对比

```
当前 (Python 耦合):
  run_single_task()
    ├── LLM.call(...)              ← 80% 耗时
    ├── call_adaptive(...)         ← 1 次 subprocess
    ├── internal_colony_search()    ← 20 次 serve.exec (全量编译)
    ├── build_adaptive_feedback()   ← 0.5 次 subprocess
    └── LLM.call(...)              ← 80% 耗时
      ↑ 各步骤在 Python 中紧耦合

目标 (分层解耦):
  Python (薄编排层):
    run_single_task():
      serve.exec("(colony:search phase expected)")
                                        ↑ 1 次调用

  Aura (控制层 lib/std/ant.aura):
    colony:search()
      ├── scan-mutables()
      ├── generate-mutations()
      ├── apply-mutation / undo-mutation  ← 纯 EDSL
      ├── pheromone:update()             ← 自适应
      └── return (found? output)          ← 1 次返回
                                        ↓ 0 次外部调用

  C++ (原语层):
    query:* / mutate:* / set-code / eval-current
```
