# evo-kv — 自进化 KV + Grok 交互设计

> **核心理念**: KV store 不是死的。它记录自己的行为模式，自动演化核心函数。
> Grok 是"大脑在环"，不是每次都需要——简单优化自闭环，复杂决策才问 Grok。

---

## 架构分层

```
┌─────────────────────────────────────────────────────┐
│                    Grok (LLM)                        │
│               通过 --serve 多 session 交互             │
└─────────────────────┬───────────────────────────────┘
                      │ send/recv (inter-agent messaging)
                      │ 或直接 EDSL (--serve exec)
                      ▼
┌─────────────────────────────────────────────────────┐
│               Layer 4: Grok Gateway                  │
│  (evo-kv-grok.aura)                                   │
│  接收 Grok 的意图 → 翻译为 EDSL 操作                  │
│  报告进化结果、异常、优化建议                          │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│            Layer 3: Evolution Engine                  │
│  (evo-kv-evolve.aura)                                 │
│  触发条件: pattern→ 使用 intend/mutate 自我修改       │
│  优化方式: 缓存策略、索引优化、TTL 自动管理            │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│         Layer 2: Introspection & Metrics              │
│  (evo-kv-metrics.aura)                                │
│  记录: 热键频率、读写比、TTL 命中率、事务大小分布      │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│           Layer 1: Core KV Store                      │
│  (evo-kv-core.aura)  ← 从 kv.aura 引出               │
│  hash + TTL + List + Set + Hash + 事务               │
└─────────────────────────────────────────────────────┘
```

---

## 交互模式

### 模式 A: Grok 主动询问 (最常见)

```
Grok → (send 'evo-kv {:cmd :status})
        ← (recv) → {:ops 1230 :hot-keys ["user:*"] :evolutions 3}

Grok → (send 'evo-kv {:cmd :evolve :target "kv-get" :strategy "cache-hot-keys"})
        ← (recv) → {:status "ok" :diff "..."
                     :before "(define (kv-get key) ...)"
                     :after  "(define (kv-get key) (or (cache-get key) ...))"}
```

### 模式 B: self-evolve 自动触发

```
(metrics-detects-hotkey "user:42" 1000 ops/s)
  → (intend "add LRU cache for hot key user:42"
       strategy: mutate-refactor
       max-attempts: 3)
  → 成功: (mutate:rebind "kv-get" "new-impl")
  → 记录到 *evo-log*
```

### 模式 C: Grok 在 --serve 直接 EDSL

```
Grok 直接发 Aura 代码：
  (set-code "(load \"projects/evo-kv/evo-kv.aura\")")
  (query:find "kv-get")                 → 找到节点
  (mutate:set-body "kv-get"             → 直接修改
    "(lambda (key) (cache-lookup key default-get key))")
  (eval-current)                         → 验证
```

---

## Evolution Engine 触发条件

| 条件 | 触发操作 | 是否需要 Grok |
|------|---------|--------------|
| 某 key 300s 内被 get >100 次 | 自动加 LRU 缓存 | ❌ 自闭环 |
| Set 操作 > Get 操作 2:1 持续 5min | 优化写入缓冲区 | ❌ 自闭环 |
| TTL 命中率 < 10% | 移除 TTL 开销 | ❌ 自闭环 |
| 事务大小平均 > 50 ops | 分片事务建议 | ✅ 问 Grok |
| 新数据类型需求（如 SortedSet） | 架构决策 | ✅ 问 Grok |
| 性能退化（ops/s 下降 > 30%） | 回滚到上一个稳定版本 | ✅ 问 Grok |
| 首次发现某类型错误 | 记录 pattern，决定是否改 API | ❌ 先问 Grok |

---

## 自进化流程详情

```
1. Metrics 层记录: *hot-key-access* = #hash
   → (kv-get "user:42") 被高频调用

2. Analysis 层判断:
   (define (analyze-hot-keys)
     (filter (lambda (entry) (> (cdr entry) 100))
             (hash->list *hot-key-access*)))

3. Evolution 层操作:
   ;; 方法 A: mutate:rebind 直接改
   (mutate:rebind "kv-get"
     "(lambda (key)
        (let ((cached (hash-ref *cache* key)))
          (if (void? cached)
              (let ((val (kv-get-original key)))
                (when (not (void? val))
                  (hash-set! *cache* key val))
                val)
              cached)))")

   ;; 方法 B: intend 让系统自动推导
   (intend "optimize kv-get for hot keys"
     strategy: mutate-refactor
     max-attempts: 3)

   ;; 方法 C: colony:search 自动搜索有效变体
   (intend "add LRU cache to kv-get with max 100 entries"
     strategy: refactor
     max-attempts: 5)
```

---

## Grok 交互协议

### ~~GroK → Evo-KV 命令格式~~

```
{:cmd :status}                    → 获取当前状态
{:cmd :metrics}                   → 获取所有监控指标
{:cmd :hkeys :threshold 100}      → 获取热键（>100 次/周期）
{:cmd :evolutions}                → 获取进化历史
{:cmd :evolve :target "kv-get"    → 触发指定函数进化
        :constraints [:speed]}
{:cmd :rollback :id 3}            → 回滚到指定进化版本
{:cmd :apply-suggestion :code ...}→ 手动应用 Grok 建议的代码
{:cmd :reset-metrics}             → 清空监控数据
```

### Evo-KV → Grok 报告格式

```
{:event :evolution
 :target "kv-get"
 :before "(define (kv-get key) ...)"
 :after  "(define (kv-get key) ...)"
 :reason "hot-key 300ops/s"
 :result "success"}

{:event :anomaly
 :type :ops-drop
 :detail "ops/s dropped 45% after evolve #3"
 :suggestion "consider rollback"}

{:event :question
 :target "evo-kv"
 :question "Should we add a new data type (SortedSet)?"
 :metrics {:sorted-set-requests 47
           :emulated-cost "O(n log n) via list"}
}
```

---

## 文件组织

```
projects/evo-kv/
├── DESIGN.md               ← 本文档
├── evo-kv-core.aura        ← Layer 1: 核心 KV（继承 kv.aura）
├── evo-kv-metrics.aura     ← Layer 2: 监控 + 分析
├── evo-kv-evolve.aura      ← Layer 3: 进化引擎
├── evo-kv-grok.aura        ← Layer 4: Grok 通信层
├── evo-kv.aura             ← 汇总导入
└── test-evo-kv.aura        ← 自测
```

---

## roadmap

### P0 — 核心 + 监控 (现在是)
- Layer 1: 从 kv.aura 核心复制 ✅
- Layer 2: 热键跟踪、读写统计 ✅
- metrics 查询原语集合 ✅

### P1 — 自进化引擎 (下一步)
- Layer 3: mutate:rebind 基础进化
- 版本回滚 (snapshot-based)
- 简单自闭环条件 (hot key → cache)

### P2 — Grok 交互 (P1 后)
- Layer 4: send/recv 网关
- Grok 可以直接 query:find + mutate:rebind
- Grok 可以接收进化报告 → 指导下一轮

### P3 — intend 驱动进化
- 用 `(intend "optimize kv-get" strategy: refactor)` 代替手写 mutate
- colony:search 自动探索有效变体
- 进化策略本身可进化

---

## Related Documentation

- [`docs/design/self-evolving-infrastructure.md`](../../docs/design/self-evolving-infrastructure.md) — broader patterns for self-evolving infrastructure beyond KV (caches, queues, schedulers, etc.). Issue #85.
- [`docs/design/double-arena.md`](../../docs/design/double-arena.md) — memory model backing the hot-swap safety
- [`docs/design/e4_evolvable_strategies.md`](../../docs/design/e4_evolvable_strategies.md) — E4 auto-tune heuristics
- [`docs/benchmark.md`](../../docs/benchmark.md) — model + benchmark for evolve-strategy validation
