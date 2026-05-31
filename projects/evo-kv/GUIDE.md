# Grok-Aura 协作指南

> 目标：Grok（LLM）与 Aura 运行时高效协作，三步一轮迭代 kv → Redis。
> 每一步：设计 → 实现（本地测试通过）→ push → 通知。

---

## 1. 协作模式

```
┌─────────────────────────────────────────────────────────────────┐
│  Grok (LLM workspace)                  │  Aura 运行时            │
│                                        │                        │
│  ① 读现状 + 设计功能                    │                        │
│  ② 写 .aura 文件                       │                        │
│  ③ 本地跑测试 ─────────────────────────→  eval，返回 stdout      │
│  ④ 看结果 → 分析失败                    │                        │
│  ⑤ 修 bug → 回到 ③                    │                        │
│  ⑥ 全绿 → git push                     │                        │
│  ⑦ 写 memory 日志                      │                        │
│  ⑧ Telegram 通知 ←──── 等待反馈 ────────┤                        │
│  ⑨ 确认后开始下一轮                     │                        │
└─────────────────────────────────────────────────────────────────┘
```

**关键改进**：
- ✅ Grok 直接在 workspace 内测试，不走 Telegram 往返
- ✅ 每次迭代包含具体失败原因和修复
- ✅ 只 push 绿线

---

## 2. Aura Lisp 规则（踩坑总结）

### 2.1 控制流

| 构造 | 行为 | 替代 |
|------|------|------|
| `(while cond body)` | 只执行 body **一次** | `letrec` 尾递归 |
| `(do ...)` | 不存在 | `letrec` |
| `(if cond then else)` | 标准条件，`#f`/`()` 为假 | — |
| `(and a b)` | 短路，返回 `#f` 或 `void` | `(if a b #f)` |
| `(when cond body...)` | 存在 | — |
| `(unless cond body...)` | 存在 | — |

**关键：`void` 在 `if` 中为真！**
```lisp
(if (void) "then" "else")  → "then"   ;; 陷阱！
(if #f "then" "else")      → "else"   ;; 正确
(if () "then" "else")      → "else"   ;; 空列表为假

;; (and ...) 只能用在使用其返回值的场景
;; 错误: (if (and (pair? x) (string=? (car x) "X")) "yes" "no")
;; 正确: (if (if (pair? x) (string=? (car x) "X") #f) "yes" "no")
```

### 2.2 字符串比较

```lisp
(equal? "a" "a")  → #f   ;; ❌
(string=? "a" "a") → #t  ;; ✅
```

### 2.3 作用域

| 问题 | 说明 | 对策 |
|------|------|------|
| `(load)` 创建新作用域 | 嵌套 load 变量不共享 | 所有文件在顶层 load |
| `define` 在函数体内 | 创建**顶层**绑定 | 用 `let` |
| 闭包捕获 | `hash-set!` 在调用链中失效 | inline 操作 |

### 2.4 Hash 操作

```lisp
(hash-set! h key val)          ;; 修改 h，返回 void
(hash-ref h key)               ;; 返回 val 或 void
(hash-has-key? h key)          ;; #t/#f
(hash-keys h)                  ;; 列表
(hash-values h)                ;; 列表
(hash-length h)                ;; 整数

;; 无 hash->list：
(map (lambda (k) (cons k (hash-ref h k))) (hash-keys h))
```

### 2.5 列表操作

```lisp
;; 存在
car, cdr, cons, list, pair?, null?, length
map, filter, foldl, reverse, take, drop
append, member, for-each, sort
list-ref, string-length, substring, number->string, string->number
inexact->exact

;; 不存在
even?, odd?
hash->list, hash-entries
symbol->string, string->symbol, string->port
assoc（自己写：见 evo-kv-admin.aura）
```

### 2.6 测试

```lisp
(check expr)              ;; 内建断言原语——不可重定义！
(check= expected actual)  ;; 内建等式检查
(define (chk a e msg) ...)  ;; ✅ 自定义测试
```

### 2.7 文档字符串

```lisp
;; ❌ 文档字符串里的 " 会关闭外层 string
;; ✅ 用方括号或避免引号
```

### 2.8 符号 = 字符串

```lisp
'foo → "foo"
'error → "error"
(keyword) → "keyword"
```

---

## 3. EDSL 交互模式

EDSL（Embedded DSL）是 Aura 原生的 AI 交互接口。
LLM 通过 EDSL 原语操作 Aura 的工作区，实现代码生成、查询和修改。

### 3.1 工作区模式

```
exec code           → parse + eval（一次性，节点 ID 不稳定）
set-code code       → 锁定到工作区（节点 ID 稳定，可多次操作）
query:* ...         → 在工作区 AST 上导航
mutate:* ...        → 修改工作区 AST
eval-current        → 执行修改后的工作区 AST
```

`exec` 适合验证一次性代码，`set-code + query + mutate + eval-current` 适合多轮迭代。

### 3.2 Query EDSL

```lisp
(query:find "fib")         → (1 5 12)       ;; 按名查找
(query:calls "fib")        → (8 15 22)      ;; 查找所有调用
(query:node-type Call)     → (0 3 8 15 22)  ;; 按类型查找
(query:children 3)         → (4 5 6)        ;; 子节点
(query:node 3)             → (Call sym:"fib" children:(4 5 6))  ;; 详情
(query:pattern "(+ n 1)")  → (12 18)        ;; 模式匹配
(query:def-use "g")        → ((def-id) . (use-ids))  ;; 定义-使用链
```

### 3.3 Mutate EDSL

```lisp
(mutate:rebind "fib" "(lambda (n) ...)" "summary")  ;; 按名替换函数
(mutate:replace-value node-id new-val "summary")     ;; 替换节点值
(mutate:set-body "fib" "new-body" "summary")         ;; 重设函数体
(mutate:insert-child parent-id pos code "summary")   ;; 插入子节点
(mutate:remove-node node-id "reason")                ;; 删除节点
```

### 3.4 策略组合

EDA 支持多种代码生成策略，分场景选用：

| 策略 | 适用场景 | 性能 | 是否需要 LLM |
|------|---------|------|------------|
| Template | boilerplate、API 包装、数据层 | 纳秒级 | ❌ |
| LLM | 新功能、复杂算法、自然语言→代码 | 2-30s | ✅ |
| Genetic | benchmark 驱动的优化 | ms 级 | ❌ |
| Pipeline | 多步骤复杂任务 | 组合 | 部分 |
| Colony | 局部变异搜索 | ms-s | ❌ |

**策略选择（规则引擎方式）**：
```lisp
;; P0 规则选择器
(cond
  ((:type :refactor :kind :rename)     → template)
  ((:type :optimize :target :speed)    → genetic)
  ((:type :new-code :complexity :high) → llm)
  ((:type :new-code :complexity :low)  → template)
  (#t → llm))
```

**混合策略 Pipeline 示例**：
```lisp
;; 实现带缓存的 HTTP 客户端
Step 1 (Template):  骨架生成 → (define (fetch url) ...)
Step 2 (LLM):       核心逻辑 → HTTP GET + parse
Step 3 (Genetic):   缓存优化 → memoization
Step 4 (LLM Fixer): 错误处理 → timeout + retry
```

### 3.5 Synthesize 原语

```lisp
(synthesize:register-template "name" "(define (handle-{name} req) ...)" "name")
(synthesize:fill "name" "my-handler")    ;; 模板实例化
(synthesize:define name sig :strategy llm :model "grok")  ;; LLM 生成
(synthesize:optimize name :population 50 :generations 100) ;; 遗传优化
```

### 3.6 Intend 原语

```lisp
(intend goal
  strategy: generate-and-fix
  max-attempts: 3)

;; 返回
'#(status: "ok"
   goal: "make fib tail-recursive"
   code: "(define (fib n) ...)"
   iterations: 3
   timeline: (...))
```

内置策略：
| 策略 | 适用场景 | 流程 |
|------|---------|------|
| `generate-and-fix` | 写新函数 | LLM 生成 → eval → 报错？→ 修正循环 |
| `refactor` | 修改现有代码 | locate → mutate → verify |
| `error-feedback-loop` | 调试修复 | typecheck → run → feedback → retry |
| `optimize` | 性能优化 | profile → suggest → apply → bench |

---

## 4. --serve 协议

`./build/aura --serve` 启动后接受 JSON 命令，每行一个 JSON 对象。

### 4.1 请求格式

```json
{"cmd":"exec","code":"(display 42)"}
```

### 4.2 响应格式

```json
{"status":"ok","value":"42"}
{"status":"error","msg":"parse error"}
{"status":"closure","msg":"program returned an uncalled function"}
```

### 4.3 命令类型

| cmd | 说明 | code 字段 |
|-----|------|-----------|
| `exec` | 执行 Aura 代码 | 任意 Aura 表达式 |
| `set-code` | 设置工作区 | 代码字符串 |
| `eval-current` | 执行工作区 | — |
| `typecheck-current` | 类型检查 | — |

### 4.4 注意事项

- `send`/`recv`/`my-id` 原语仅在 `--serve` 模式下有效
- 响应行以 `\n` 分隔，输出和 JSON 分开
- JSON 是响应末尾的最后一个 `{...}` 块
- 超时 15s，超时后 serve 会被重启

---

## 5. 控制策略（Control Strategy Taxonomy）

### 5.1 什么时候用什么方法

| 场景 | 方法 | 原因 |
|------|------|------|
| 新增 Redis 命令 | 直接写 `.aura` + 测试 | 确定性实现，不需要 LLM |
| 修 bug | 本地 test → fix → retest | 闭环快，ms 级 |
| 优化性能 | `synthesize:optimize` | 自动化 benchmark |
| 添加新数据类型 | 设计文档 + 逐步实现 | 需要架构决策 |
| 自动修复运行时错误 | `intend error-feedback-loop` | LLM 看错误信息修正 |

### 5.2 当前 evo-kv 演化策略

```
R1-R4: Grok 直接写
  确定性实现，不需要 LLM 生成。
  模式：设计 → 写代码 → 测试 → fix → push

R5-R6: Grok 写 + 部分 auto
  Pub/Sub 需要 serve 模式才能跑。
  AOF 需要文件 I/O + 增量写入。

R7: Benchmark 套件
  引入 EDSL benchmark 模式：
  task 定义 → LLM 生成 → serve 执行 → 验证

R8+: LLM 辅助
  RESP 协议层：LLM 生成协议解析代码
  Replication：Grok 设计 + LLM 生成
```

---

## 6. Prompt 模式参考

### 6.1 功能实现 Prompt

```
实现 <功能>：
- 文件名：evo-kv-xxx.aura
- 要求：<Redis 兼容性要求>
- 接口：<API 签名>
- 测试：<测试用例>
```

### 6.2 Bug 修复 Prompt

```
Bug：<现象>
期望：<期望行为>
实际：<实际输出>
根因分析：<分析>
修复方案：<方案>
```

### 6.3 EDSL benchmark Task 格式

```lisp
;; task: <task-name>
;; goal: <what to do>
;; expect: <expected output>
;; depend: <required modules>
;; hint: <implementation hint>
```

### 6.4 Grok 决策请求（auto engine → Grok）

当 auto 引擎无法自修时，升级到 Grok 的问题队列：

```lisp
{:type "hot-key"
 :key "user:42"
 :ops 60
 :question "Should we add LRU cache for this hot key?"
 :options ["LRU-100" "LRU-1000" "no-cache"]}
```

---

## 7. API 规范参考

### 7.1 数据结构约定

```lisp
;; Hash → k=v 列表
(define (hash->list h)
  (map (lambda (k) (cons k (hash-ref h k))) (hash-keys h)))

;; Assoc 查找（字符串键）
(define (assoc key alist)
  (letrec ((search (lambda (items)
                     (if (null? items) #f
                         (if (string=? (car (car items)) key) (car items)
                             (search (cdr items)))))))
    (search alist)))
```

### 7.2 循环模式

```lisp
;; for i from 0 to n-1:
(letrec ((loop (lambda (i)
                 (when (< i n)
                   ;; body using i
                   (loop (+ i 1))))))
  (loop 0))
```

### 7.3 错误返回约定

```lisp
"OK"                   ;; 成功（无返回值）
"ERR wrong type"       ;; 类型错误
"ERR no such key"      ;; 键不存在
"ERR wrong number of arguments"  ;; 参数错误
(cons 'OK results)     ;; 事务成功
(error? expr)          ;; 检查是否错误
```

### 7.4 惰性求值模式

```lisp
;; 条件执行（避免不必要的计算）
(if cond (begin exp1 exp2) exp3)
(when cond body)
(unless cond body)
```

---

## 8. evo-kv 编码规范

### 8.1 命名

```
*evo-store*          — 全局 hash 表用 * 包裹
*evo-ttl*            — 全局可变状态
evo-get              — 公共函数用 evo- 前缀
zadd                 — Redis 兼容命令直接用原名
track-evo-get        — 带监控的包装器
zset-get-ordered     — 内部辅助函数用 zset- 前缀
```

### 8.2 文件组织

```
evo-kv-core.aura     — 基础 KV
evo-kv-metrics.aura  — 监控层
evo-kv-cache.aura    — LRU 缓存层
evo-kv-evolve.aura   — 进化引擎
evo-kv-grok.aura     — Grok 通信网关
evo-kv-auto.aura     — 自动驱动循环
evo-kv-zset.aura     — Sorted Set
evo-kv-admin.aura    — 管理命令
test-*.aura          — 测试文件
DESIGN.md            — 设计文档
ROADMAP.md           — 演进路线图
GUIDE.md             — 本文档
```

### 8.3 加载顺序

```lisp
;; ✅ 正确：在顶层依次 load
(load "projects/evo-kv/evo-kv-core.aura")
(load "projects/evo-kv/evo-kv-metrics.aura")
(load "projects/evo-kv/evo-kv-cache.aura")
(load "projects/evo-kv/evo-kv-evolve.aura")
(load "projects/evo-kv/evo-kv-grok.aura")
(load "projects/evo-kv/evo-kv-auto.aura")
(load "projects/evo-kv/evo-kv-zset.aura")
(load "projects/evo-kv/evo-kv-admin.aura")
```

### 8.4 测试规范

- 不嵌套 load
- 用 `chk`（非 `check`）
- 提供 `;; goal:` / `;; expect:` 元数据

---

## 9. 迭代节奏

```
R1  LRU 缓存                      13/13  ✅
R2  Auto 驱动                     13/13  ✅
R3  Sorted Set                    15/15  ✅
R4  逐出策略 + INFO                8/8    ✅
R5  Pub/Sub                        ⬜
R6  AOF 持久化                     ⬜
R7  Benchmark 套件                 ⬜
R8  RESP 协议层                    ⬜
R9  Replication / 集群            ⬜
```

每轮流程：设计 → 实现 → 测试 → 修复(循环) → 提交 → push → 写日志 → 通知

---

## 10. 新增功能 Checklist

- [ ] 功能文件 `.aura`
- [ ] 测试文件 `test-xxx.aura`
- [ ] 主测试脚本包含新文件
- [ ] 本地 `./build/aura` 全绿
- [ ] ROADMAP.md 更新
- [ ] GUIDE.md 更新（踩坑记录）
- [ ] git push
- [ ] 写 memory/YYYY-MM-DD.md
