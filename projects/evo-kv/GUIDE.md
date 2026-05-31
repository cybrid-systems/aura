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

**关键改进**（对比最初的手动流程）：
- ✅ Grok 直接在 workspace 内测试，不走 Telegram 往返
- ✅ 每次迭代包含具体失败原因和修复
- ✅ 只 push 绿线

---

## 2. Aura Lisp 规则（踩坑总结）

### 2.1 控制流

| 构造 | 行为 | 替代 |
|------|------|------|
| `(while cond body)` | 只执行 body **一次** | 用 `letrec` 尾递归 |
| `(do ((i 0 (+ i 1))) ((= i n)) body)` | 不存在 | 用 `letrec` |
| `(if cond then else)` | 标准条件，`#f`/`()` 为假 | — |
| `(and a b)` | 短路，返回 `#f` 或 `void` | `(if a b #f)` 确保返回 `#f` |
| `(when cond body...)` | 存在，条件真时执行 | — |
| `(unless cond body...)` | 存在，条件假时执行 | — |

**关键：`void` 在 `if` 中为真！**
```lisp
(if (void) "then" "else")  → "then"   ;; 陷阱！
(if #f "then" "else")      → "else"   ;; 正确
(if () "then" "else")      → "else"   ;; 空列表为假

;; 所以 (and ...) 不能直接用在 if 条件里
;; 错误: (if (and (pair? x) ...) "yes" "no")
;; 正确: (if (if (pair? x) (string=? (car x) "...") #f) "yes" "no")
```

### 2.2 字符串比较

```lisp
(equal? "a" "a")  → #f   ;; ❌ 不行！
(string=? "a" "a") → #t  ;; ✅ 用这个
```

### 2.3 作用域

| 问题 | 说明 | 对策 |
|------|------|------|
| `(load)` 创建新作用域 | 嵌套 load 时变量不共享 | 所有文件在顶层 load，永不在文件内嵌套 |
| `define` 在函数体内 | 创建**顶层**绑定（非局部） | 用 `let` 代替，或避免在函数内 define |
| 函数闭包捕获 | `hash-set!` 在调用链中失效 | inline 操作，不要透过中间函数传 hash |

### 2.4 Hash 操作

```lisp
;; hash-set!/hash-remove! 是原地修改，返回 void
(hash-set! h key val)          ;; 修改 h，返回 void
(hash-ref h key)               ;; 返回 val 或 void（key 不存在时）
(hash-has-key? h key)          ;; 返回 #t/#f
(hash-keys h)                  ;; 返回所有 key 的列表
(hash-values h)                ;; 返回所有 value 的列表
(hash-length h)                ;; 返回条目数

;; 没有 hash->list！用以下方式实现
(map (lambda (k) (cons k (hash-ref h k))) (hash-keys h))
```

### 2.5 列表操作

```lisp
;; 存在
car, cdr, cons, list, pair?, null?, length
map, filter, foldl, reverse, take, drop
append, member
for-each, sort (比较函数)

;; 不存在
even?, odd?
hash->list, hash-entries
symbol->string, string->symbol
string->port
```

### 2.6 测试

```lisp
(check expr)              ;; 内建断言原语——不可重定义！
(check= expected actual)  ;; 内建等式检查

;; 自定义测试函数，不要叫 check！
(define (check-eq a e msg) ...)    ;; ✅ 正确
(define (chk a e msg) ...)         ;; ✅ 正确
```

### 2.7 文档字符串

```lisp
;; ❌ 文档字符串里的 " 会关闭外层 string
(define (f x)
  "this has \"quotes\" inside")  ;; 陷阱！

;; ✅ 用别的方式表达
(define (f x)
  "this uses square brackets [quotes] inside")
```

### 2.8 符号 = 字符串

```lisp
'foo      → "foo"    ;; 符号就是字符串
'error    → "error"  ;; 符号求值为字符串
(keyword) → "keyword" ;; 关键字也是字符串
```

---

## 3. evo-kv 编码规范

### 3.1 命名

```
*evo-store*          — 全局 hash 表用 * 包裹
*evo-ttl*            — 全局可变状态
evo-get              — 公共函数用 evo- 前缀
evo-set              — 操作函数
zadd                 — Redis 兼容命令直接用原名
track-evo-get        — 带监控的包装器
zset-get-ordered     — 内部辅助函数用 zset- 前缀
```

### 3.2 文件组织

```
evo-kv-core.aura     — 基础 KV（hash/TTL/List/Set/Hash/事务）
evo-kv-metrics.aura  — 监控层（热键/读写比/延迟）
evo-kv-cache.aura    — LRU 缓存层
evo-kv-evolve.aura   — 进化引擎（版本快照/回滚）
evo-kv-grok.aura     — Grok 通信网关
evo-kv-auto.aura     — 自动驱动循环
evo-kv-zset.aura     — Sorted Set
test-*.aura          — 测试文件
DESIGN.md            — 设计文档
ROADMAP.md           — 演进路线图
GUIDE.md             — 本文档
```

### 3.3 加载顺序

```lisp
;; ✅ 正确：在顶层依次 load，永不嵌套
(load "projects/evo-kv/evo-kv-core.aura")
(load "projects/evo-kv/evo-kv-metrics.aura")
(load "projects/evo-kv/evo-kv-cache.aura")
(load "projects/evo-kv/evo-kv-evolve.aura")
(load "projects/evo-kv/evo-kv-grok.aura")
(load "projects/evo-kv/evo-kv-auto.aura")
(load "projects/evo-kv/evo-kv-zset.aura")
(load "projects/evo-kv/test-zset.aura")
```

### 3.4 测试规范

每个 .aura 功能文件配一个 `test-xxx.aura`：
- 不嵌套 load（在顶层由测试脚本统一 load）
- 使用 `chk`（不是 `check`，避免和内建冲突）
- 包含正例和反例
- 提供 `;; goal:`、`;; expect:` 元数据（兼容 EDSL benchmark）

---

## 4. 迭代节奏（R4 起）

```
Round   内容                          测试通过数  预计耗时
─────────────────────────────────────────────────────────
R1      LRU 缓存                      13/13      ← 完成
R2      Auto 驱动                     13/13      ← 完成
R3      Sorted Set                    15/15      ← 完成
R4      逐出策略 + INFO                ~18        30 min
R5      Pub/Sub + slo LOG              ~22        30 min
R6      AOF 持久化                     ~25        30 min
R7      Benchmark 套件                 ~30        30 min
R8      RESP 协议层                    ~35        45 min
R9      Replication / 集群            ~40        60 min
```

每轮迭代流程：
1. 读 ROADMAP.md + 当前代码
2. 设计：写该功能的设计思路（3-5 行）
3. 实现：写 `.aura` + `test-xxx.aura`
4. 测试：`cat test_all.aura | ./build/aura`
5. 修复：检查输出，修 bug，重复 4
6. 提交：全绿 → `git commit -m "evo-kv: R<N> — 功能"`
7. git push
8. 写 memory 日志
9. 通知用户

---

## 5. 新增功能的 Checklist

- [ ] 功能文件 `.aura`
- [ ] 测试文件 `test-xxx.aura`
- [ ] 主测试脚本包含新文件
- [ ] 本地 `./build/aura` 全绿
- [ ] ROADMAP.md 更新进度
- [ ] GUIDE.md 新增踩坑记录
- [ ] git push
- [ ] 写 memory/YYYY-MM-DD.md
