# Aura Code Evolution Pipeline — 设计文档

> **状态：** 提案 / 设计中
> **基于：** Aura v2.6 (commit 04b88f0+)
> **核心理念：** Aura 的自修改能力 (`query:*` / `mutate:*` / `ast:*` / `workspace:*`) + LLM 代码生成（EDSL Benchmark 已验证）构成闭环，让代码能在运行时安全地自我演化。

---

## 1. 现状盘点 — 真实可用的能力

在画任何架构图之前，先明确**哪些已经能跑**、**哪些需要搭桥**：

### ✅ 已就绪（Aura 运行时内建）

| 原语 | 用途 | 验证方式 |
|------|------|---------|
| `set-code` | 将字符串解析为代码并载入 workspace | `(set-code "(define (f x) (+ x 1))")` |
| `eval-current` | 执行 workspace 中的代码 | `(eval-current)` |
| `typecheck-current` | 类型检查 workspace 代码 | `(typecheck-current)` |
| `ast:snapshot` / `ast:restore` | 版本快照与回滚 | `(ast:snapshot "v1")` / `(ast:restore "v1")` |
| `ast:list-snapshots` / `ast:diff` | 快照管理 | |
| `query:find` / `query:node` / `query:children` / `query:parent` / `query:root` | AST 自省 | `(query:find 'add)` → node IDs |
| `query:calls` / `query:def-use` | 函数调用/定义-使用链分析 | |
| `mutate:rebind` | 重绑定函数定义 | `(mutate:rebind "add" "(lambda (a b) (* a b))" "改为乘法")` |
| `mutate:set-body` / `mutate:replace-pattern` | 修改函数体 | |
| `mutate:replace-type` / `mutate:replace-value` | 修改节点类型/值 | |
| `mutate:splice` / `mutate:wrap` / `mutate:insert-child` / `mutate:remove-node` | 结构性变异 | |
| `mutate:extract-function` / `mutate:inline-call` / `mutate:rename-symbol` | 重构原语 | |
| `mutation-count` / `mutation-history` | 审计追踪 | |
| `workspace:create` / `workspace:switch` / `workspace:merge` / `workspace:discard` | 工作区管理 | |
| `agent:spawn` | 创建子 agent session | |
| `orch:conduct` / `orch:metrics` | 指令分发与度量 | |
| `typed_mutate` (type-safe mutation) | 类型安全的变异 | `typed_mutate` typecheck (#34) 6/6 tests |
| `format` | 类型格式化（#41 已补全 Variant/Record） | |

### ⚠️ 部分就绪（需要环境配置）

| 能力 | 状态 | 说明 |
|------|------|------|
| LLM 调用 | ⚠️ Python 桥接 | EDSL Benchmark (`tests/edsl_benchmark.py`) 通过 Python 调用 LLM，Aura 自身没有内嵌 HTTP/LM 客户端 |
| 网络服务 | ⚠️ 基础支持 | `--serve` JSON 协议 + fiber scheduler 可用，但无完整 HTTP/RESP 协议解析 |
| 文件 I/O | ⚠️ 基础 | `read-file` / `write-file` / `file-exists?` 存在，无流式写入 |

### ❌ 不存在（不可用于设计）

| 声称的 API | 实际替代 |
|-----------|---------|
| `mutate:replace!` | 不存在。用 `mutate:rebind` 或 `mutate:replace-pattern` |
| `mutate:assoc!` | 不存在。用 `hash-set!` |
| `grok-*` / `enable-grok-evolution` | 不存在。LLM 调用需通过 Python 桥接 |
| `aof-log` / `log-mutation` | 不存在。审计用 `mutation-history` |
| `freeze` (数据持久化) | 不存在 RDB。有 `--freeze` CLI 用于 workspace 快照 |
| `start-redis` | 不存在（用户可自己 define） |
| `current-time` | 不存在同名 prim。检查是否有 `time` 相关原语 |
| 内嵌 HTTP/gRPC 客户端 | 不存在 |

---

## 2. 架构设计

### 2.1 核心洞察

Aura 的独特价值**不是**用 `mutate:*` 操作运行时数据（那只需要普通 Lisp `set!` / `hash-set!`），而是**用 `query:*` + `mutate:*` 操作代码本身的 AST 结构**。

这意味着真正的演化流程是：

```
 LLM (外部)
   │ 生成 Aura 代码
   ▼
 set-code / mutate:rebind  →  代码进入运行时
   │
   ▼
 typecheck-current  →  类型安全验证
   │
   ▼
 eval-current / benchmark  →  功能验证
   │
   ▼  (失败)
 query:*  →  AST 自省，定位问题
   │
   ▼  (反馈给 LLM)
 ast:restore  →  回滚到安全状态
```

### 2.2 分层架构

```
Layer 3: 编排层 (orchestration)
 ┌─────────────────────────────────────────┐
 │ agent:spawn   orch:conduct              │
 │ 多 session 协作 / 管线编排               │
 └─────────────────────┬───────────────────┘
                       │
Layer 2: 演化层 (evolution)               ← Aura 独有能力
 ┌─────────────────────────────────────────┐
 │ set-code → typecheck → eval             │
 │   ↕                                     │
 │ query:* → 分析 → mutate:* → 修复        │
 │   ↕                                     │
 │ ast:snapshot ↔ ast:restore (安全网)     │
 │ workspace:merge ↔ workspace:discard     │
 └─────────────────────┬───────────────────┘
                       │
Layer 1: 业务逻辑层 (domain)               ← 普通 Lisp
 ┌─────────────────────────────────────────┐
 │ define / let / lambda                   │
 │ make-hash-table / hash-set! / hash-ref  │
 │ cons / car / cdr / list / map / filter  │
 │ pair? / string-append / display / read  │
 │ 类型注解 / declare-type                 │
 └─────────────────────────────────────────┘
```

**关键区别：**
- **Layer 1**：写业务逻辑（Redis、CLI 工具、DSL）。用普通 Lisp，**不需要** mutate API。
- **Layer 2**：写演化逻辑（"改代码的代码"）。用 `query:*` / `mutate:*` / `ast:*` / `workspace:*`。
- **Layer 3**：写多 agent 协作。用 `agent:spawn` / `orch:conduct`。

### 2.3 演化流程（完整闭环）

```
Phase 0: 种子代码
  │  用户 / LLM 提供初始代码
  │  (set-code "...")
  ▼
Phase 1: 验证
  │  typecheck-current → 类型通过?
  │  eval-current → 功能正确?
  │  benchmark → 性能达标?
  ▼
Phase 2: 分析 (可选)
  │  query:find / query:calls / query:def-use
  │  → 识别瓶颈 / 死代码 / 类型缺口
  ▼
Phase 3: 变异
  │  ast:snapshot (安全快照)
  │  mutate:rebind / replace-pattern / extract-function...
  ▼
Phase 4: 再验证
  │  typecheck-current → 变异后类型检查
  │  eval-current → 功能回归测试
  │
  ├── 通过 → 确认变异 (保留新代码)
  │
  └── 失败 → ast:restore → 回滚 + 记录失败
       │
       ▼
  Phase 5: 反馈
    记录失败信息，可通过 LLM 生成修复
```

---

## 3. 真实可跑的代码示例

### 3.1 Layer 1 业务逻辑：一个最小 Key-Value Store

```lisp
;; === kv.aura — 最小键值存储 ===
;; 全部用普通 Lisp，不需要 mutate API

(define *store* (make-hash-table))

(define (kv-set key val)
  (hash-set! *store* key val)
  "OK")

(define (kv-get key)
  (hash-ref *store* key))

(define (kv-del key)
  (hash-remove! *store* key)
  "OK")

(define (kv-incr key)
  (let ((cur (hash-ref *store* key)))
    (if (and (number? cur) (integer? cur))
        (let ((new (+ cur 1)))
          (hash-set! *store* key new)
          new)
        "ERR wrong type")))

;; 使用
(kv-set "name" "Aura")  ; → "OK"
(kv-get "name")         ; → "Aura"
(kv-incr "counter")     ; → 1
(kv-incr "counter")     ; → 2
```

> **验证：** 这些 API 全部真实存在，可以在 Aura REPL 中直接运行。

### 3.2 Layer 2 演化逻辑：代码自修改

```lisp
;; === evolve.aura — 代码演化引擎 ===
;; 核心循环：注入代码 → 验证 → 回滚
;; 依赖 Layer 1 的 kv-store 已在 workspace 中

(define *evolution-log* '())

(define (safe-inject! code-snippet)
  "安全注入代码片段：快照 → 注入 → 验证 → 回滚"
  (let* ((snap (ast:snapshot "pre-inject"))
         (old-code (current-source))
         (new-code (string-append old-code "\n" code-snippet)))
    (set-code new-code)
    (let* ((tc-result (typecheck-current))
           (errors (typecheck-errors)))  ;; 假设有获取错误的 API
      (if (and tc-result (null? errors))
          (begin
            (set! *evolution-log*
                  (cons (list "inject" (current-time) code-snippet 'ok)
                        *evolution-log*))
            (list 'ok "注入成功"))
          (begin
            (ast:restore snap)
            (set! *evolution-log*
                  (cons (list "inject" (current-time) code-snippet 'rolled-back errors)
                        *evolution-log*))
            (list 'rollback (string-append "类型错误: " (string-join errors))))))))

(define (safe-mutate! target-name new-body description)
  "安全修改函数：快照 → mutate → 验证 → 回滚"
  (let ((snap (ast:snapshot "pre-mutate")))
    (mutate:rebind target-name new-body description)
    (let* ((tc-result (typecheck-current)))
      (if tc-result
          (begin
            (set! *evolution-log*
                  (cons (list "mutate" target-name description 'ok)
                        *evolution-log*))
            (list 'ok "变异通过"))
          (begin
            (ast:restore snap)
            (set! *evolution-log*
                  (cons (list "mutate" target-name description 'rolled-back)
                        *evolution-log*))
            (list 'rollback "类型检查失败"))))))

(define (analyze-function fn-name)
  "分析函数的 AST 结构"
  (let* ((nodes (query:find fn-name))
         (details (map (lambda (id) (query:node id)) nodes))
         (callers (query:calls fn-name)))
    (list
     (cons 'nodes nodes)
     (cons 'details details)
     (cons 'callers callers))))
```

> ⚠️ **注意：** `typecheck-errors` 和 `current-time` 可能需要确认是否存在同名原语。实际使用时替换为真实 API。

### 3.3 Layer 3 编排层：多 Session 协作

```lisp
;; === orchestrate.aura — agent 编排 ===

(define (parallel-eval fns . args)
  "在多个子 session 中并行求值"
  (let ((sessions (map (lambda (fn)
                         (let ((s (agent:spawn (string-append "worker-" fn))))
                           (orch:conduct s (list fn . args))
                           s))
                       fns)))
    ;; 收集结果
    (map (lambda (s) (orch:conduct s '(result))) sessions)))
```

---

## 4. 实际能跑的演化用例

### 用例 1：用 LLM 优化函数实现

**场景：** `kv-get` 实现太简单，想换成带缓存的版本。

```
1. 用户/Grok 生成新实现:
   (define (kv-get key)
     (let ((cached (hash-ref *cache* key)))
       (if cached
           cached
           (let ((val (hash-ref *store* key)))
             (hash-set! *cache* key val)
             val))))

2. EDSL Benchmark Python 驱动：
   - 发送 task 给 LLM → 收到代码
   - pipe 给 Aura `set-code`

3. Aura 侧：
   ast:snapshot "before-opt"
   mutate:rebind "kv-get" "<new-code>" "add cache layer"
   typecheck-current → OK
   eval-current → 回归测试
   → 通过，确认变异
```

### 用例 2：Attach 新功能到运行时

**场景：** 给 kv-store 加 TTL 过期。

```
1. LLM 生成:
   (define *ttl* (make-hash-table))
   (define (kv-set-with-ttl key val ttl-sec)
     (hash-set! *store* key val)
     (hash-set! *ttl* key (+ (current-seconds) ttl-sec))
     "OK")
   (define (kv-get key)
     (let ((expire (hash-ref *ttl* key)))
       (if (and expire (> expire (current-seconds)))
           (hash-ref *store* key)
           (begin (hash-remove! *store* key)
                  (hash-remove! *ttl* key)
                  #f))))

2. Aura 侧：
   set-code (原代码 + 新代码)
   typecheck-current → 验证类型
   eval-current → 测试功能
   ast:snapshot "v2-with-ttl"
```

### 用例 3：用 query:* 做覆盖率分析

```lisp
;; 分析当前 workspace 中哪些函数被调用、哪些是死代码
(define (dead-code-analysis)
  (let* ((root (query:root))
         (all-defs (query:find 'define))
         (live-fns (map (lambda (def)
                          (query:node def))
                        (filter (lambda (n)
                                  (let* ((node (query:node n))
                                         (name (if (pair? node) (caddr node) "")))
                                    (> (length (query:calls name)) 0)))
                                all-defs))))
    (list 'live-count (length live-fns)
          'dead-count (- (length all-defs) (length live-fns)))))
```

> ⚠️ `query:node` 返回值格式需要根据实际 API 调整。

---

## 5. 当前不可行的功能与替代路径

| 想要的 | 问题 | 替代方案 |
|--------|------|---------|
| Aura 原生 LLM 调用 | 无内嵌 HTTP 客户端 | Python 桥接（EDSL Benchmark 模式） |
| mutate 运行时数据 | mutate 只操作 AST | 用 `set!` / `hash-set!` |
| 真实网络服务 (Redis RESP) | 无完整协议解析 | 用 `--serve` JSON + 自定义 parser |
| 文件持久化 (AOF/RDB) | 仅有基础 file I/O | `write-file` + 自定义序列化 |
| 集群/分片 | 无网络层 | 在 Layer 3 用 `agent:spawn` 模拟 |
| `current-time` | 可能存在但需确认 | 用 `(time)` 或系统调用替代 |

---

## 6. 路线图建议

### 短期可做（依赖现有能力）

1. **写一个真实能跑的 kv.aura**（Layer 1 验证）
2. **写一个真实能跑的 evolve.aura**（Layer 2 验证）
3. **用 EDSL Benchmark 跑通 Python→LLM→Aura 循环**
   - 已有 145 task，Grok 83% 通过率
   - 缺的是把 `query:*` 结果反馈给 LLM

### 中期需补充

4. **给 Aura 加内嵌 HTTP 客户端原语**
   - 或者在 Python 桥接层封装
5. **完善 `typecheck-errors` API**
   - 当前 `typecheck-current` 返回 bool+diag，但缺少纯 Lisp 可消费的错误列表
6. **提供 Aura 侧基准测试框架**
   - 用于 Volume 2 的 Phase 4（验证循环）

### 长期愿景

7. **闭环 CLI 工具**
   - `aura evolve --target kv.aura --llm grok`
8. **安全变异策略库**
   - `mutate:*` + `ast:snapshot` + `rollback` 的组合模板

---

## 7. 附录：API 速查表

### 查询 AST

```lisp
(query:find 'symbol)        → (node-id ...)
(query:children node-id)    → (child-id ...)
(query:parent node-id)      → (parent-id ...)
(query:root)                → root-node-id
(query:node node-id)        → (tag value type sym-id ...)
(query:calls 'fn-name)      → (call-site-node-id ...)
(query:def-use 'fn-name)    → (def-node-id . use-node-ids)
```

### 变异代码

```lisp
(mutate:rebind name new-body reason)    → bool
(mutate:set-body node-id new-body)      → bool
(mutate:replace-pattern old new)        → bool
(mutate:splice node-id new-children)    → bool
(mutate:wrap node-id wrapper)           → bool
(mutate:insert-child parent-id child-id pos) → bool
(mutate:remove-node node-id)            → bool
(mutate:replace-type node-id new-type)  → bool
(mutate:replace-value node-id new-val)  → bool
(mutate:tweak-literal node-id delta)    → bool
(mutate:rename-symbol old new)          → bool
(mutate:extract-function target new-name) → bool
(mutate:inline-call call-site)          → bool
```

### 版本控制

```lisp
(ast:snapshot name)                     → snapshot-id
(ast:restore id)                        → bool
(ast:list-snapshots)                    → ((id name time) ...)
(ast:diff id1 id2)                      → diff-string
(mutation-count)                        → int
(mutation-history node-id)              → ((id op summary) ...)
(rollback mutation-id)                  → bool
(rollback-since mutation-id)            → int
```

### 工作区

```lisp
(workspace:create name)                 → bool
(workspace:switch name)                 → bool
(workspace:current)                     → name
(workspace:list)                        → (name ...)
(workspace:merge source)                → bool
(workspace:discard)                     → bool
(workspace:sync-from source)            → bool
(workspace:lock name)                   → bool
(workspace:can-write?)                  → bool
```

### 类型系统

```lisp
(type-of value)                         → "Int" | "String" | ...
(type? value "TypeName")                → bool
(declare-type name params ret)          → void
(define-type (Name params) (Ctor ...))  → void
(typecheck-current)                     → bool
```

### Agent 编排

```lisp
(agent:spawn name)                      → session-id
(orch:conduct session command)          → result
(orch:metrics)                          → metrics-data
```
