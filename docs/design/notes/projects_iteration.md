# Project-Driven Core Iteration — 设计文档

> **注意（历史文档）**：本文档描述了项目驱动迭代的原始理念。`projects/` 目录已移除（commit `2882e37`）。当前迭代方式见 [roadmap.md](../roadmap.md)；实装以 `tests/suite/`、`lib/std/` 和 `src/` 为准。

> **核心理念：** 不凭空设计功能。写真实项目 → 暴露核心短板 → 修复核心 → 写更难的项目。
> 每写一个 `projects/` 下的 demo，至少发现并修复 3-5 个 Aura 核心问题。

---

## 1. 为什么用 Project-Driven 方式

### 过去的问题

之前 Aura 的功能开发方式是**功能驱动**（"加 type registry"、"加 gradual typing"），好处是系统性强，坏处是：

- 功能做完了不知道好不好用（没有真实用户场景验证）
- API 设计容易脱离实际需求（比如 `register_type` 和 `register_variant` 两个 API 的边界不清晰）
- 缺少端到端的"手感"反馈

### Project-Driven 的方式

```
写项目 ──→ 发现卡点 ──→ 修核心 ──→ 写更难的项目
  │            │            │
  │  API 不好用  │  编译器bug  │  新增 prim
  │  缺少 prim   │  类型系统   │  修复边界
  │  性能瓶颈    │  运行时     │  ...
  ▼            ▼            ▼
记录到 gap log → 规划修复 → 实现 → 验证（项目能跑了）
```

### 核心原则

1. **每个项目必须真实可用** — 能在 Aura REPL 中加载并执行
2. **每个项目暴露的 gap 必须回修到 core** — 不修 core 就换 API
3. **gap 修复优先于新功能** — 不堆新功能，先让已有的能干活
4. **projects/ 是测试场** — 代码质量可以迭代，不需要一步到位

---

## 2. 项目目录结构

```
projects/
├── README.md           ← 每个项目的简要说明、状态、暴露的 gap
├── GAPS.md             ← 累积的 gap 清单（core 尚未修复的）
│
├── kv/                 # 最小键值存储
│   ├── kv.aura         # 核心实现
│   └── test-kv.aura    # 自测脚本
│
├── cli/                # CLI 工具（参数解析 + 文件处理）
│   ├── cli.aura
│   └── ...
│
├── todo/               # TODO 应用（数据结构 + 持久化）
│   ├── todo.aura
│   └── ...
│
├── chat/               # 简单聊天协议（serve + mailbox）
│   └── ...
│
└── calc/               # 表达式计算器 + 插件系统
    └── ...
```

### 项目状态标记

每个项目在 README 中标注：

```
状态: [planning] [writing] [works] [core-gaps-filed]
核心缺口: #gap-xxx, #gap-yyy
```

---

## 3. 项目规划与暴露的 Gap

### P1: KV Store (最小键值存储)

**定位：** 最简单的"数据库" — 只依赖 hash table + 字符串。

**功能目标：**
```lisp
(kv-set "key" "val")    → "OK"
(kv-get "key")          → "val"
(kv-del "key")          → "OK"
(kv-incr "counter")     → 1 2 3...
(kv-exists? "key")      → #t / #f
(kv-keys)               → ("key1" "key2" ...)
(kv-flush)              → "OK"
```

**预期暴露的 core gaps：**

| # | Gap | 严重度 | 修复方向 |
|---|-----|--------|---------|
| G1 | `hash-remove!` 返回值不明确 | 🔴 使用体验 | 确认语义并补文档/测试 |
| G2 | 缺少 `hash-keys` 遍历接口 | 🟡 功能缺失 | 检查是否存在，不存在则加 |
| G3 | 没有 `current-time` / `time` prim | 🟡 TTL 需要 | 加系统时间 prim（或确认替代） |
| G4 | `typecheck-current` 错误信息无法从 Lisp 消费 | 🔴 演化层关键缺口 | 暴露 error list 接口 |
| G5 | 类型检查只返回 bool，没有结构化结果 | 🟠 Layer 2 受阻 | 设计 `typecheck-result` 类型 |
| G6 | 缺少 `string-join` / `string-split` | 🟡 字符串处理 | 确认是否已存在 |

### P2: CLI 工具

**定位：** 参数解析 + 文件 I/O + 流程控制。

**功能目标：**
```lisp
;; 一个能跑的命令行 todo 工具
(todo-add "buy milk")
(todo-list)
(todo-done 1)
(todo-save "todos.json")
(todo-load "todos.json")
```

**预期暴露的 core gaps：**

| # | Gap | 严重度 | 修复方向 |
|---|-----|--------|---------|
| G7 | 缺少 JSON 序列化 | 🟠 实用性 | 加 `json-encode` / `json-decode` |
| G8 | 命令行参数解析能力弱 | 🟡 开发体验 | 提供 `command-line` 规范 |
| G9 | 缺少 `string->number` 可空返回值处理 | 🟡 错误处理 | 文档明确 |

### P3: Chat Protocol

**定位：** 用 `--serve` + `mailbox` 做 session 间消息。

**功能目标：**
```lisp
;; session A
(send-msg "session-B" "hello")
;; session B
(recv-msg)  → "hello"
```

**预期暴露的 core gaps：**

| # | Gap | 严重度 | 修复方向 |
|---|-----|--------|---------|
| G10 | `mailbox` prim 未完整暴露 | 🔴 功能缺失 | 确认并补全 mailbox API |
| G11 | `--serve` 协议缺少路由能力 | 🟠 扩展性 | 设计 serve 路由 |

### P4: Expression Calculator + Plugin System

**定位：** 利用 `mutate:*` 做真正的代码演化——运行时插件加载。

**功能目标：**
```lisp
(calc "1 + 2 * 3")      → 7
;; 用户可以在运行时"安装"新操作符
(install-plugin 'power '(lambda (x n) (^ x n)))
(calc "2 power 3")      → 8
```

**核心价值：** 这才是 `mutate:*` 的正确使用场景——不是改数据，而是**运行时添加新语法/操作符**。

**预期暴露的 core gaps：**

| # | Gap | 严重度 | 修复方向 |
|---|-----|--------|---------|
| G12 | 运行时添加函数绑定后，已有代码不感知 | ⚠️ 设计问题 | 研究 `env_` 动态绑定机制 |
| G13 | `eval` 在当前环境的隔离问题 | 🟠 沙箱 | 确认 `eval` 的行为边界 |

---

## 4. Iteration Cycle

每个项目的标准迭代周期：

```
Week 1: 项目起步
  Day 1-2:  写出项目骨架（纯 Lisp，跑通基础路径）
  Day 3-4:  运行 → 记录所有卡点到 GAPS.md
  Day 5:    评估哪些卡点需要回修 core

Week 2: 核心修复
  按优先级修复 GAPS.md 中的 core gaps：
    🔴 阻塞型 — 不修项目无法继续
    🟠 重要型 — 影响可用性
    🟡 改善型 — 可以绕过

Week 3: 项目收尾
  用修复后的 core 重写/优化项目代码
  验收 → 更新状态为 [works]
  🔄 开始下一个项目
```

### 优先级规则

| 优先级 | 条件 | 操作 |
|--------|------|------|
| P0 | 项目完全无法写 | 立即修 |
| P1 | 项目能写但很难用 | 本周内修 |
| P2 | 项目能用但有别扭的 workaround | 记录，下个项目前修 |
| P3 | "如果有个 xxx 就好了" | 记录到 backlog |

---

## 5. GAP 跟踪规范

### GAP 条目格式（在 GAPS.md 中）

```markdown
## G4: typecheck-current 错误信息不可从 Lisp 消费

发现于: kv/ 项目 (2026-05-30)
严重度: 🟠
场景:   (typecheck-current) 只返回 bool，
        但 Layer 2 演化逻辑需要具体错误信息来决定回滚还是修正

当前行为:
  (typecheck-current) → #t 或 #f

期望行为:
  (typecheck-current) → (values ok? (error-list ...))

修复方案:
  1. 在 TypeChecker 中暴露 diagnose() 结果
  2. 新 prim: (typecheck-errors) → (("msg" line col) ...)
  3. 集成到 safe-inject! 流程中

状态: [open] / [in-progress] / [fixed] / [wontfix]
关联 PR: #xx
```

---

## 6. 与现有路线图的关系

```
现有 P 系列 (核心)           Projects 系列 (验证)
─────────────────           ─────────────────
P2.6 AOT                   P1 kv-store (暴露运行时 gap)
P2.7 原语闭包               P2 cli-tool (暴露 stdlib gap)
P2.8 ???                    P3 chat-proto (暴露 serve gap)
P3 完整类型系统              P4 calc-plugin (暴露 mutate gap)
P4 标准库补全                ...

         ↑ 修复流向 ↑
  projects 发现的 gap → 修 core → 功能更强
```

**projects/ 不取代 P 系列**。projects/ 是**验证场**，P 系列是**核心能力规划**。projects 中发现的 gap 按优先级进入 P 系列的迭代。

---

## 7. 起步

### 第一步：建目录 + 写 README

```
mkdir -p projects/kv
```

### 第二步：写 kv.aura（依赖已确认的 API）

```lisp
;; projects/kv/kv.aura — 最小键值存储
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
    (if (integer? cur)
        (let ((new (+ cur 1)))
          (hash-set! *store* key new)
          new)
        "ERR wrong type")))

(define (kv-exists? key)
  (not (eq? (hash-ref *store* key) #f)))

(define (kv-keys)
  ... )  ;; ← 这里就会遇到 Gap: hash-keys 存在吗？

(define (kv-flush)
  (set! *store* (make-hash-table))
  "OK")
```

### 第三步：尝试运行

```bash
echo '(load "projects/kv/kv.aura")(kv-set "x" "1")(kv-get "x")' | ./build/aura
```

### 第四步：记录所有卡点到 GAPS.md

每遇到一个"不知道这个 API 在不在"、"这个返回值有点奇怪"、"缺了这个功能"，就记一条 gap。

---

## 8. 预期成果

| 指标 | 预期 |
|------|------|
| 写完 P1-P4 后新增/修复的 core prim | 10-15 个 |
| projects/ 中可运行的 demo | 4 个 |
| GAPS.md 中已修复的 gap | >80% |
| 写 P4 时的体验 vs P1 | 显著更流畅 |
