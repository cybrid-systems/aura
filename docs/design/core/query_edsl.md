# Query + Transform EDSL — 设计文档

> **Status (2026-06-07, Issue #112):** ✅ EDSL 已远超本文档最初规划。`query:where` / `query:filter` 已实装；`DefUseIndex` 已与 mutate 集成；`mutate:query-and-replace`（#110）将 query + replace 组合为单步原子操作。本文档已同步到当前代码状态。
>
> **Audience:** AI Agent（LLM）—— 通过少量 S-表达式在 Aura AST 上做精确的增量操作。
> **Human reader:** 见 [docs/developer/evaluator.md](../developer/evaluator.md) 了解如何给 evaluator 加新原语。

---

## 0. Implementation Status (2026-06-11, Issue #154)

**重要**：本文档描述的 query 能力 **高于实际 Aura 表层实装**。准确分两层：

### C++ Core Layer (`src/compiler/evaluator_impl.cpp`)

| 原语 | 实装 | 备注 |
|------|------|------|
| `query:find` | ✓ | 按名称找节点（Define/Variable 等） |
| `query:node-type` | ✓ | 按 NodeTag 过滤节点 |
| `query:children` | ✓ | 节点的子节点 ID 列表 |
| `query:parent` | ✓ | 节点的父节点 ID |
| `query:siblings` | ✓ | 同父节点的兄弟节点 |
| `query:root` | ✓ | 工作区根节点 ID |
| `query:node` | ✓ | 节点详情（标签 + 子节点） |
| `query:calls` | ✓ | 引用了某名称的所有节点 |
| `query:where` | ✓ | 构造谓词描述子（:node-type / :callee / :defined-by / :parent-type / :has-type） |
| `query:filter` | ✓ | 按谓词过滤节点 ID 列表（AND 语义） |
| `query:pattern` | ✓ | S-表达式模式匹配（含 `...` 通配符） |
| `query:def-use` | ✓ | def-use 路径查询（C++ 内部使用，DefUseIndex） |

全部 11 个原语在 C++ 层 **实装**。可以直接从 Aura 通过 `evaluator` 内部接口调用。

### Aura Helper Layer (`lib/std/query.aura`)

| 原语 | 实装 | 备注 |
|------|------|------|
| `query:filter` | ✓ | 简单列表过滤（基于 lambda 谓词，AND 不支持多谓词组合） |
| `query:uncalled` | ✓ | 找出所有未被调用的 Define |
| `query:callers-of` | ✓ | 找出所有调用某名称的节点 ID |
| `query:find` / `query:calls` / `query:node-type` / `query:pattern` / `query:where` | ✗ | **Aura 层未包装**——需要直接调用 C++ 内部接口 |

`lib/std/query.aura` 只有 **3 个 helper**（filter、uncalled、callers-of）。文档中演示的其它原语需要通过 C++ 内部接口调用（AI Agent 可以但 Aura 代码不行）。

### Rich EDSL Querying 的未来工作

- 完整的 `(query:where :field value)` 谓词 DSL 仍可扩展（更多谓词类型）
- 高级模式匹配（嵌套 `...`、guard 表达式）—— `query:pattern` 当前只支持扁平 `...`
- 全功能 DefUse 查询的 Aura 表面 —— C++ 内部有完整的 DefUseIndex，Aura 表面只暴露了 `refers-to?` helper

**AI Agent 读者请注意**：本文档作为设计意图保留。但实装代码状态以本文档为准。**Aura 代码**只能使用 `lib/std/query.aura` 中实装的 helper；**C++ 代码**可以直接调用 `evaluator.ixx` 中所有 11 个原语。

### Code References（实现位置）
- 主要注册：`src/compiler/evaluator_impl.cpp` 
  - `query:find` ~L6036, `query:where` ~L6483, `query:pattern` ~L6725, `query:def-use` ~L9696 等。
  - QueryEngine 类在 `src/compiler/query.ixx` / `query_impl.cpp`（支持 where 谓词、pattern matching、DefUseIndex）。
- 与 mutate 集成：`mutate:query-and-replace` ~L5154。
- 增量/CaaS：与 `CompilerService` cache 和 dirty 标记集成（见 §4）。
- 开发者扩展：`docs/developer/evaluator.md`。

（注：JIT 加速比会随具体 workload 和优化迭代变化，最新实测以对应 benchmark 和 §0 表格为准。）

---

## 1. 核心模型：工作区

```
  exec code             ← 临时求值，每次新建 AST（节点 ID 不稳定）

  set-code code         ← 锁定 AST 到工作区（节点 ID 稳定）
  query:* ...           ← 在工作区 AST 上导航
  mutate:* ...          ← 修改工作区 AST
  eval-current          ← 执行修改后的工作区 AST

  exec code             ← 回到临时求值模式
```

**关键区别**：
- `exec` 是"读-执行-丢"，适合验证一次性代码
- `set-code` + `query` + `mutate` + `eval-current` 是"锁定-导航-修改-执行"，适合多轮迭代

工作区内节点 ID 保证跨操作稳定（直到下一次 `set-code` 或 `exec` 重新解析）。`query:*` / `mutate:*` 都接受 `NodeId`（int）作为参数。

---

## 2. Query EDSL —— 现状

### 2.1 按名称查找

```scheme
; 查找所有叫 fib 的定义/变量
(query:find "fib")         → (1 5 12)  ; 节点 ID 列表

; 查找所有调用 fib 的地方
(query:calls "fib")        → (8 15 22)

; 查找所有类型为 Call 的节点
(query:node-type Call)     → (0 3 8 15 22)

; 查找所有 LiteralInt 节点
(query:node-type LiteralInt) → (4 7 10 18)
```

### 2.2 AST 导航

```scheme
; 查看节点的子节点
(query:children 3)         → (4 5 6)    ; 子节点 ID

; 查看父节点
(query:parent 6)           → (3)        ; 父节点 ID (可能多个)

; 查看节点详情
(query:node 3)             → (Call sym:"fib" children:(4 5 6))

; 查看相邻兄弟节点
(query:siblings 5)         → (4 6)

; 工作区根
(query:root)               → 0
```

### 2.3 按模式搜索

```scheme
; 查找所有 (+ n 1) 模式的调用
(query:pattern "(+ n 1)")  → (12 18)

; 使用 `...` 通配符匹配任意子树
(query:pattern "(fib ...)") → (15 22)  ; 所有调用 fib 的位置
(query:pattern "(if ... ... ...)") → (19)  ; 所有 if 表达式

; `...` 是 Ellipsis token，由 lexer 识别三个连续的点号
```

### 2.4 谓词 + 过滤（#110 后实装）

```scheme
; (where :field value) — 构造一个谓词描述子
;   (where :node-type "Call")
;   (where :callee "sort")
;   (where :defined-by "fib")
;   (where :parent-type "Lambda")
;   (where :has-type "Int")
;
; 返回一个不透明的 tagged pair，query:filter 知道如何求值。

; (query:filter predicate ...) — 组合谓词（AND 语义）
(query:filter
  (where :node-type "Call")
  (where :callee "sort"))     → (8 15)    ; 所有调用 sort 的节点

; 也可以单独使用 where 拿到谓词描述子，配合未来的 query:where-on
```

### 2.5 DefUseIndex 查询（#107 part 5 + 后续）

```scheme
; 按符号名查 def → use 链
(query:def-use "fib")     → ((21) . (23 6 12))   ; (defs . uses)

; 从 def 出发查所有 use 点
(query:reaches 21)        → ((21) . (23 6 12))

; def → use + callers
(query:effects "fib")     → ((21) (23 6 12) (25 17 11))

; 索引统计
(query:index-stats)       → ((stale-syms 0) (defuse-version 7))
```

`DefUseIndex` 是 workspace AST 之上的派生索引，由 `defuse_affected_syms_`
+ `defuse_touch_fn_` 协议维护（详见 [docs/developer/evaluator.md §4](../developer/evaluator.md#4-defuseindex-touch-protocol)）。

### 2.6 AST 反射（#108 part 2 实装）

```scheme
; 当前工作区中所有顶层 Define
(ast:defs)                → (("fib" . 21) ("square" . 35))

; 所有节点 ID（按 flat 顺序）
(ast:nodes)               → (0 1 2 3 4 5 6 ...)

; 版本号（每次 set-code / mutate +1）
(ast:version)             → 7

; 节点类型
(query:type 3)            → "Int"
```

### 2.7 输出格式

所有 query 返回值为 Aura 列表：

```scheme
(query:find "fib")         → (1 5 12)
(query:children 3)         → (4 5 6)
(query:node 3)             → ("Call" "fib" (4 5 6))
```

空结果返回空列表 `()`。错误返回 tagged pair `("error" . ("kind" . "message"))`。

---

## 3. Transform EDSL —— 现状

### 3.1 按函数名替换（稳定）

```scheme
; 替换整个函数定义（按名查找，不需要节点 ID）
(mutate:rebind "fib"
  "(define (fib n)
     (if (< n 2) n
       (+ (fib (- n 1)) (fib (- n 2)))))"
  "递归版 fib")

; 预期输出: → ok
```

### 3.2 按节点 ID 修改（精确定位）

```scheme
; 替换整数字面量（配合 query 先定位）
(mutate:replace-value node-id new-value "summary")

; 替换类型注解
(mutate:replace-type node-id "Int" "summary")

; 记录操作（不影响 AST）
(mutate:record-patch node-id "op-name" "summary")

; 微调字面量（自动推断类型）
(mutate:tweak-literal node-id delta "summary")    ; n → n+delta
```

### 3.3 原子结构操作

```scheme
; 删除节点
(mutate:remove-node node-id "remove dead code")

; 插入子节点（解析到工作区，保留所有已有节点 ID）
(mutate:insert-child parent-id position child-code "add parameter")
; 返回新插入节点 ID
; 例: 向 Call (+ x y) 的 children[1] 插入 (* x x)
(mutate:insert-child 3 1 "(* x x)" "insert mul")
; → children 变成 (0 9 1 2): [+, new_call_9, x, y]

; 重设函数体（不改变参数签名）
(mutate:set-body "fib"
  "(iter 0 1 n)"
  "fib 改成迭代")

; 替换节点值（自动适配节点类型）
(mutate:replace-value node-id new-int   "summary")  ; LiteralInt → int
(mutate:replace-value node-id new-float "summary")  ; LiteralFloat → float
(mutate:replace-value node-id "new-name" "summary")  ; Variable/LiteralString → string

; 包裹节点（在父节点位置加一层）
(mutate:wrap node-id wrapper-code "summary")
; 例: (mutate:wrap 3 "(display _)" "wrap")
;   把 3 号节点的父位置改成 (display 3-子树)

; 拼接（在指定位置插入新节点）
(mutate:splice parent-id position child-code... "summary")
; 例: (mutate:splice 0 1 "(display 1)" "(display 2)" "insert")
;   向 0 节点的子节点列表 position 1 位置插入多个新节点
```

### 3.4 高层重构（2026-Q2 实装）

```scheme
; 重命名符号（自动遍历所有使用点）
(mutate:rename-symbol "old-name" "new-name" "summary")

; 提取为函数（自动收集自由变量作参数）
(mutate:extract-function node-id "func-name" "summary")

; 内联调用（用 body 替换 call site）
(mutate:inline-call call-node-id "summary")

; 移动子树到新父节点
(mutate:move-node node-id new-parent-id position "summary")
```

### 3.5 Query + Replace 组合（#110 —— `mutate:query-and-replace`）

```scheme
; 把所有调用 (+ ...) 替换为 (y ...)
(mutate:query-and-replace
  (query:where :callee "+")
  "y"
  "linearize adds")

; 用模板模式 + `...` 占位做局部替换
(mutate:query-and-replace
  (query:where :callee "foo")
  "(bar ...)"
  "rename foo→bar")
```

这是把 query 和 replace 组合为单步原子操作的关键原语。详见
[docs/design/history/closings/110-closing.md](../history/closings/110-closing.md)。

> ⚠️ **使用警示**：写一个 combine（read + write）循环的 mutate 时，**必须**在循环
> 外 snapshot `end_id = flat.size()`。详见
> [docs/developer/evaluator.md §1](../developer/evaluator.md#1-the-self-modifying-flat-iteration-rule-issue-111-lesson)。

---

## 4. CaaS (增量编译) 集成 —— 现状

```
  exec code              → parse + lower + execute (全量)
  set-code code          → parse + store AST (不执行)

  mutate:rebind ...      → 只标记被修改的节点
  mutate:replace-value   → 只标记被修改的节点

  eval-current           → 只重新编译被标记的子树：
                            1. 找到所有被标记的 Define/Let/Expr
                            2. 重新类型检查（仅增量）
                            3. 重新求值
                            4. 清除标记
```

**实装状态（2026-06）**：
- ✅ `CompilerService::set_code()` + `eval_current()` 在 IR V2 cache
  (`source-hash → ir_module`) 之上工作。`pre_cache_workspace_defines_fn_`
  在 set-code 后预热所有顶层 define 的 cache。
- ✅ EDSL V2 dirty 标记：每个 define 在 set-code 后按 source-hash 判定
  是否脏；`mark_define_dirty_fn_` / `mark_all_defines_dirty_fn_` 在
  mutate 时被调用。
- ✅ `eval-current :jit` 走 LLVM ORC JIT 路径（38 opcode → native，~7.55× 加速）。
- 🟡 **尚未实装**：mutate 之后的"只重新编译被 dirty 的子树"的子树级
  增量（当前是 source-hash compare 之后全量 re-lower dirty define，
  对几百行程序 ~1-5ms，全量够用）。

实际性能：`eval-current` ~1-5ms（几千节点程序）。瓶颈不在 C++，在
LLM 调用（5-30s 一次）。

---

## 5. 类型系统集成

```scheme
; 验证当前 AST 类型正确性
(typecheck-current)       → (("ok") / ("error" "message"))

; 查询节点的类型
(query:type 3)            → "Int"

; mutate 时自动验证类型
(mutate:rebind "fib" ...)  → 如果类型不匹配，返回错误
```

**实装状态（2026-06）**：
- ✅ `typecheck-current` 已通过 `Evaluator::run_typecheck_no_lock`
  实现，从 Aura 端直接调用。
- ✅ `query:type` 由 `TypeResolutionIndex` 支持（M2.7）。
- ✅ `mutate:*` 自动验证类型（`mutate:rebind` / `mutate:replace-value`
  等在 type mismatch 时返回 `("error" . ("type-mismatch" . ...))`）。
- ✅ `mutate:rebind` / `mutate:set-body` 在类型变化时调用
  `defuse_affected_syms_.insert(name)` + `defuse_touch_fn_()`，
  `DefUseIndex` 自动失效。

---

## 6. 性能估算（实装值）

假设 AST 大小 500-5000 节点（典型 Aura 程序）：

| 操作 | 复杂度 | 实际时间 |
|------|--------|----------|
| `query:find "fib"` | O(VariableNodes) | ~10μs |
| `query:children N` | O(1) | ~0.1μs |
| `query:calls "fib"` | O(CallNodes) | ~5μs |
| `query:pattern "(+ n 1)"` | O(Nodes × PatternDepth) | ~50μs |
| `query:filter` (N 谓词) | O(Nodes × N) | ~50-200μs |
| `query:def-use` (cached) | O(1) | ~1μs |
| `query:def-use` (rebuild) | O(Nodes × Symbols) | ~100-500μs |
| `mutate:rebind "fib" ...` | O(Find + Rebuild) | ~1ms（含增量编译）|
| `mutate:replace-value` | O(1) + MutationLog | ~50μs |
| `mutate:query-and-replace` | O(Nodes × Predicates) | ~100-500μs |
| `eval-current` | 全量重编译 + 缓存命中 | ~1-5ms |
| `typecheck-current` | 全量类型检查 | ~1-5ms |
| `ast:snapshot` / `ast:restore` | O(flat 深度拷贝) | ~100-500μs |

瓶颈不在 C++，在 LLM 调用（5-30s 一次）。

---

## 7. 实现优先级（实装状态）

```
P0 — 核心 EDSL（LLM 可用）✅
  1. ✅ query:find / query:children / query:node / query:calls 原语
  2. ✅ set-code / eval-current 原语
  3. ✅ mutate:rebind（按函数名替换）
  4. ✅ mutate:replace-value（支持任意节点类型）

P1 — 完整 EDSL ✅
  5. ✅ query:parent / query:siblings / query:root
  6. ✅ query:pattern（模式匹配搜索）
  7. ✅ mutate:set-body / mutate:remove-node / mutate:insert-child
  8. ✅ typecheck-current 原语
  9. ✅ query:type
 10. ✅ mutate:wrap / mutate:splice / mutate:tweak-literal

P2 — 高级 EDSL ✅
 11. ✅ query:filter / query:where（组合谓词）
 12. ✅ DefUseIndex + query:def-use / query:reaches / query:effects
 13. ✅ ast:snapshot / ast:rollback / ast:version（#107 part 3 + 6）
 14. ✅ per-sym staleness（#107 part 5）
 15. ✅ ast:defs / ast:nodes（#108 part 2）

P3 — 高级重构 ✅
 16. ✅ mutate:extract-function / mutate:inline-call
 17. ✅ mutate:rename-symbol / mutate:move-node
 18. ✅ mutate:refactor/extract (alias)
 19. ✅ mutate:query-and-replace（#110）

P4 — 增量编译 🟡
 20. ✅ EDSL V2 source-hash cache（set-code 后预热）
 21. ✅ dirty marking on mutate（mark_define_dirty_fn_）
 22. 🟡 子树级增量（当前是 source-hash compare 后 re-lower dirty define）
 23. 🟡 增量类型检查（只检查被修改的子树）

P5 — 自动修复 🟡
 24. 🟡 AutoFixEngine（rule-based 修复管线）
 25. 🟡 MutationLog Aura API
```

---

## 8. --serve 协议扩展

```json
// 设置工作区 AST
{"cmd":"exec","code":"(set-code \"...\")"}

// Query 命令 — 在工作区 AST 上查询
{"cmd":"exec","code":"(query:find \"fib\")"}

// Mutate 命令 — 修改工作区 AST
{"cmd":"exec","code":"(mutate:rebind \"fib\" \"...\")"}

// 执行当前工作区
{"cmd":"exec","code":"(eval-current)"}
```

所有 query/mutate 操作都是 Aura 原语，通过现有的 `exec` 命令发送。
Serve 协议本身不变。新增的 `query:where` / `mutate:query-and-replace` 等
高级原语都通过 `exec` 通道，无需协议升级。

---

## 9. 设计决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| Query 原语 vs JSON 命令 | Aura 原语 | 统一复用 --serve 协议，不增加新命令 |
| 按名查找 vs 节点 ID | 两者都支持 | 名稳定但模糊，ID 精确但可能变 |
| 工作区 vs 只在 exec | 工作区 | exec 每次新建 AST，query 没有意义 |
| 全量 eval vs 增量 | 先全量，再 EDSL V2 缓存 | 代码量小，全量够快；EDSL V2 解决了重复全量 |
| query:filter 是 AND 还是 OR | AND | 多谓词同时成立 = 精确匹配；OR 太宽泛 |
| mutate:query-and-replace 是 C++ 还是 Aura | C++ | 性能 + atomicity；Aura 层做 2 步会暴露中间态 |
| AST 版本号 vs git hash | AST 版本号 | hash 不稳定，version 单调递增即可 |
| DefUseIndex 全量 vs per-sym | per-sym（#107 part 5） | mutate 后只重 build 受影响的 sym，避免全量 reindex |

---

## 10. Agent 集成模式

### 10.1 基础 query → mutate 循环

```scheme
;; 1. 设代码
(set-code "(define (bad-fact n) (* n (bad-fac (- n 1))))")

;; 2. 定位问题
(define bad-call (car (query:find "bad-fac")))  ; → 5

;; 3. 修复
(mutate:rebind "bad-fact"
  "(define (bad-fact n) (if (= n 0) 1 (* n (bad-fact (- n 1)))))"
  "fix typo")

;; 4. 验证
(eval-current)
(display (bad-fact 5))    ; → 120
```

### 10.2 影响范围分析

```scheme
(set-code "
  (define (render-loop xs)
    (map (lambda (x) (* x 2)) xs))
  (define (main)
    (display (render-loop (list 1 2 3))))
")

; Agent 想知道：改 render-loop 的 body 会影响哪些使用点？
(query:effects "render-loop")
; → ((render-loop . def) (main . use) (...))

; Agent 想知道：变量 x 在哪定义、哪使用？
(query:def-use "x")
; → ((4) . (5 6))      ; def 在 4，use 在 5 6
```

### 10.3 安全 mutate（snapshot → mutate → verify → rollback）

```scheme
(define snap (ast:snapshot "before-try"))
(mutate:rebind "fib" "(lambda (n) (* n 2))" "linearize")

(if (= 0 (eval-current-output))
  (begin
    (display "regression!")
    (ast:restore snap))   ; 回退到快照
  (display "ok, keep changes"))
```

`ast:snapshot` / `ast:restore` 在 #107 part 6 之后是 O(1) deep-copy，
保存 SymId / mutation_log / type_id / value_cache 的全部状态。

---

## 11. 相关文档

- [docs/developer/evaluator.md](../developer/evaluator.md) — evaluator C++ 实现细节
- [docs/design/history/closings/107-closing.md](../history/closings/107-closing.md) — workspace mutex + AST versioning
- [docs/design/history/closings/110-closing.md](../history/closings/110-closing.md) — qar + self-modifying-flat 教训
- [docs/design/history/closings/111-closing.md](../history/closings/111-closing.md) — self-modifying-flat 审计
- [docs/design/defuse_analysis.md](defuse_analysis.md) — DefUseIndex 内部设计
- [docs/tutorial.md §10](../tutorial.md) — EDSL / AI Agent 开发 tutorial
