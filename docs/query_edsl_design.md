# Query + Transform EDSL — 设计文档

为 AI Agent（LLM）设计的 AST 查询和变换语言。
目标：LLM 通过少量 S-表达式在 Aura AST 上做精确的增量操作。

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

工作区内节点 ID 保证跨操作稳定（直到下一次 `set-code` 或 `exec` 重新解析）。

---

## 2. Query EDSL

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
```

### 2.3 按模式搜索

```scheme
; 查找所有 (+ n 1) 模式的调用
(query:pattern "(+ n 1)")  → (12 18)

; 查找所有递归调用（函数体内调用自身）
(query:pattern "(fib ...) inside (define (fib ...))")
                           → (15)
```

### 2.4 输出格式

所有 query 返回值为 Aura 列表：

```scheme
(query:find "fib")         → (1 5 12)
(query:children 3)         → (4 5 6)
(query:node 3)             → ("Call" "fib" (4 5 6))
```

空结果返回空列表 `()`。

---

## 3. Transform EDSL

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
```

### 3.3 原子操作

```scheme
; 删除节点
(mutate:remove-node node-id "remove dead code")

; 插入子节点
(mutate:insert-child parent-id position child-code "add parameter")

; 重设函数体（不改变参数签名）
(mutate:set-body "fib"
  "(iter 0 1 n)"
  "fib 改成迭代")
```

---

## 4. CaaS (增量编译) 集成

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

当前状态：`CompilerService` 已有 `set_code()` + `eval_current()`。
缺失：增量 dirtiness 标记 + 增量类型检查。

评估：增量编译短期内可以不实现全量。先用 `set-code` + `eval-current` 做全量重新编译
（对几百行程序足够快）。增量 dirtiness 标记留到有性能需求时再加。

---

## 5. 类型系统集成

进行 mutate 后，类型系统需要做验证：

```scheme
; 验证当前 AST 类型正确性
(typecheck-current)       → (("ok") / ("error" "message"))

; 查询节点的类型
(query:type 3)            → "Int"

; mutate 时自动验证类型
(mutate:rebind "fib" ...)  → 如果类型不匹配，返回错误
```

当前 `CompilerService` 有 `typecheck(input)` 方法但没暴露到 `--serve` 协议。
需要加一个 `--serve` 命令或 Aura 原语来调用。

简要实现：在 `--serve` 协议中添加 `"cmd":"typecheck"` 命令，
或在 Aura 中注册 `(typecheck expr)` 原语。

---

## 6. 性能估算

假设 AST 大小 500-5000 节点（典型 Aura 程序）：

| 操作 | 复杂度 | 实际时间 |
|------|--------|----------|
| `query:find "fib"` | O(VariableNodes) | ~10μs |
| `query:children N` | O(1) | ~0.1μs |
| `query:calls "fib"` | O(CallNodes) | ~5μs |
| `query:pattern "(+ n 1)"` | O(Nodes × PatternDepth) | ~50μs |
| `mutate:rebind "fib" ...` | O(Find + Rebuild) | ~1μs |
| `mutate:replace-value` | O(1) + MutationLog | ~0.5μs |
| `eval-current` | 全量重新编译 | ~1-5ms |
| `typecheck-current` | 全量类型检查 | ~1-5ms |

瓶颈不在 C++，在 LLM 调用（5-30s 一次）。

---

## 7. 实现优先级

```
P0 — 立即（核心 EDSL，LLM 可用）
  1. 注册 query:find / query:children / query:node / query:calls 原语
  2. 注册 set-code / eval-current 原语
  3. 注册 mutate:rebind（按函数名替换）
  4. 修复 mutate:replace-value 使其支持任意节点类型（不限于 LiteralInt）

P1 — 短期（完整 EDSL）
  5. query:parent / query:siblings
  6. query:pattern（模式匹配搜索）
  7. mutate:set-body / mutate:remove-node
  8. typecheck-current 原语

P2 — 中期（增量编译）
  9. Dirtiness 标记（被 mutate 修改的节点）
  10. 增量类型检查（只检查被修改的子树）
  11. 增量求值（只重新执行被修改的函数）

P3 — 长期
  12. query:filter / query:where（组合查询）
  13. MutationLog 查询和回滚的 Aura 原语
  14. AutoFixEngine 自动修复规则
```

---

## 8. --serve 协议扩展

新增 JSON 命令：

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
不需要扩展 `--serve` 协议本身——只需要注册新的 Aura 原语。

---

## 9. 设计决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| Query 原语 vs JSON 命令 | Aura 原语 | 统一复用 --serve 协议，不增加新命令 |
| 按名查找 vs 节点 ID | 两者都支持 | 名稳定但模糊，ID 精确但可能变 |
| 工作区 vs 只在 exec | 工作区 | exec 每次新建 AST，query 没有意义 |
| 全量 eval vs 增量 | 先全量 | 代码量小，全量够快；增量以后加 |
