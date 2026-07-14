# API 表面

**完整原语列表以代码为准**，不要依赖本页的静态枚举。

运行时：`(api-reference)` → 主列表 + `*deprecated*` 段  
静态扫描：[generated/primitives.md](generated/primitives.md)（`./build.py docs`）  
Agent 一页提示：[agent-prompt-template.md](agent-prompt-template.md) · 教程：[tutorial.md](tutorial.md)

---

## 规范入口（Canonical — 新代码只用这些）

| 集群 | 规范 API | 说明 |
|------|----------|------|
| 加载 / 执行 | `set-code` `current-source` `eval-current` | 工作区装载与执行 |
| 结构查询 | **`(query :op …)`** | #1435 · 见下表 |
| 变异 | **`(mutate :op …)`** | #1436 · 见下表 |
| 工作区 | **`(workspace :op …)`** | #1437 · 见下表 |
| 版本 | `ast:snapshot` `ast:restore` `rollback` | 快照 / 回滚 |
| 观测 | **`(engine:metrics …)`** | #1433 · 勿新增 `*-stats` 名 |
| 产品 convenience | `(require "std/surface" all:)` | string/json/math |

> **Stable refs (#393)**：跨 `mutate` 保存的节点引用请用  
> `(query :children id :stable #t)` / `(query :parent id :stable #t)`  
> （或旧名 `query:children-stable` / `query:parent-stable`）。  
> 裸 `NodeId` 在 mutation 后可能失效。

### `(query :op …)`

```scheme
(query :node id)
(query :children id)                 ; 裸 NodeId 列表
(query :children id :stable #t)      ; (id . gen)
(query :parent id)
(query :parent id :stable #t)
(query :find "name")
(query :def-use "var")
(query :mutation-log)                ; 可选 n
(query :root)
```

高级（仍可用、非 deprecated）：`query:pattern` `query:filter` `query:calls` …

### `(mutate :op …)`

```scheme
(mutate :rebind "f" "(lambda (x) (* x 2))" "summary")
(mutate :replace target :pattern|:subtree|:value|:type …)
(mutate :move node new-parent idx)
(mutate :extract node-id "name")
(mutate :validate code-string schema)
(mutate :atomic (list …))            ; atomic-batch
```

### `(workspace :op …)`

```scheme
(workspace :create "name")           ; → id
(workspace :switch id)
(workspace :merge id)
(workspace :lock id)
(workspace :unlock id)
(workspace :list)
(workspace :current)
```

### `(engine:metrics …)`

```scheme
(engine:metrics)                     ; schema 2 + 分组 hash
(engine:metrics :group "jit")
(engine:metrics :prefix "query:")
(engine:metrics "query:pattern-stats")  ; 过渡期按名单
(require "std/engine-metrics" all:)
```

`mutate:atomic-batch` 语义细节见 [design/core/mutate_api.md](design/core/mutate_api.md)。

---

## Deprecated（仍注册 — 兼容旧脚本）

运行时完整列表：`(api-reference)` 输出中的 `*deprecated*` 段。

| 旧名 | 改用 |
|------|------|
| `query:node` `query:children` `query:parent` `query:find` `query:def-use` `query:mutation-log` … | `(query :op …)` |
| `query:children-stable` `query:parent-stable` | `(query :children/:parent id :stable #t)` |
| `mutate:rebind` `mutate:replace-*` `mutate:move-node` `mutate:extract-function` `mutate:atomic-batch` … | `(mutate :op …)` |
| `workspace:create` `workspace:switch` `workspace:merge` `workspace:lock` `workspace:unlock` … | `(workspace :op …)` |
| 任意 `query:*-stats` / `compile:*-stats`（top-20 已标 deprecated） | `(engine:metrics …)` |
| `mutate:sv-*` | 扩展面（计划迁出内核） |

**冻结（P0b / #1432）**：禁止新增 `*-stats`、`string-*`/`json-*`/…、`ast:ref-*` 公共名。

静态文档中带 `**deprecated**` 标记的名见 [generated/primitives.md](generated/primitives.md)。

---

## 观测补充

```scheme
(compile:snapshot)                               ; → hash（8 keys）
(hash-ref (compile:snapshot) "marker-user-count")
(hash-ref (engine:metrics "query:pattern-stats") "schema")
```

注意：`(stats:get …)`（`lib/std/stats.aura`）是**按名字调度 primitive**，不是 hash 访问器；读 facade 键请用 `hash-ref`。

---

## Agent 入口

- JSON：`./build/aura --serve-async` — [wire-formats.md](wire-formats.md)
- 编排：`lib/std/orchestrator.aura` · `tests/suite/orchestrator.aura`
- 一页提示：[agent-prompt-template.md](agent-prompt-template.md)

## 标准库

索引：[generated/stdlib-index.md](generated/stdlib-index.md) · 源文件 `lib/std/*.aura`

常用：`(require "std/surface" all:)` `(require "std/list" all:)` `(require "std/engine-metrics" all:)` `(require "std/orchestrator" all:)`

## 开发者

加 primitive：[contributing.md](contributing.md) · 模块地图：[architecture.md](architecture.md)
