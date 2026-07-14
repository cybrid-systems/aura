# API 表面

**完整原语列表以代码为准**，不要依赖本页的静态枚举。

> **Deprecation (Issue #393)**: 原始 `(query:children <id>)` /
> `(query:parent <id>)` 返回裸 `NodeId`，跨 mutating 调用
> 可能失效。AI agent 代码请改用
> `(query:children-stable <id>)` /
> `(query:parent-stable <id>)`（返回 `(id . gen)` pair）。
> C++ 端请用 `FlatAST::children_stable()` / `parent_stable()`。
> 详见 [design/core/stable_ref_best_practices.md](design/core/stable_ref_best_practices.md)
> §“TL;DR — raw `query:children` / `query:parent` are deprecated”
> 以及 §“Cross-mutate storage guidelines (Issue #393)”。

## 运行时查询

```scheme
(api-reference)    ; REPL 或 eval 中调用 → 全部已注册 primitive 名称
```

实现：`evaluator_primitives_eval.cpp` 中 `primitives_.add("api-reference", …)`，遍历 `Primitives` 槽位生成。

**Issue #1434 / P1b**：top-20 高频 `query:*-stats` 在 `PrimMeta.deprecated = true` 后，
`api-reference` 会单独输出 `*deprecated*` 段（推荐 `(engine:metrics "query:…")` /
`(require "std/engine-metrics" all:)`），不再混在主列表中。排序与名单见
`scripts/find_top_stats.py`（`--print-pinned`）。

静态扫描（CI 校验）：[generated/primitives.md](generated/primitives.md) — `./build.py docs`

## 自修改 EDSL（摘要）

| 集群 | 代表原语 |
|------|----------|
| 加载 | `set-code` `current-source` `eval-current` `eval-current-output` |
| Query | **`(query :op …)`**（#1435 规范入口）· `query:pattern` · `query:calls` · … |
| Mutate | `mutate:rebind` `mutate:query-and-replace` `mutate:extract-function` `mutate:atomic-batch` … |

### `(query :op …)` — Issue #1435

规范调度（旧 `query:node` / `query:children` / … 仍注册，标 deprecated）：

```scheme
(query :node id)
(query :children id)                    ; 裸 NodeId 列表
(query :children id :stable #t)         ; (id . gen) — Issue #393
(query :children-stable id)             ; 同上
(query :parent id)
(query :parent id :stable #t)
(query :find "name")
(query :def-use "var")
(query :mutation-log)                   ; 可选 n
(query :root)                           ; 便捷
```
| 验证 | `query:schema` `mutate:validate-against-schema` |
| 版本 | `ast:snapshot` `ast:restore` `rollback` `mutation-history` |
| Workspace | `workspace:create` `workspace:switch` `workspace:merge` `workspace:lock` … |

示例：[tutorial.md](tutorial.md) §EDSL · `tests/suite/mutate-structured.aura`

`mutate:atomic-batch` 强原子性、`(atomic-batch:stats)` 可观测字段与并发 fiber 语义见 [design/core/mutate_api.md](design/core/mutate_api.md)。

## 观测原语（hash 形态）

**首选 facade（#1433 / #1434）**：

```scheme
(engine:metrics)                              ; schema 2 + compile/jit/… 分组
(engine:metrics "query:pattern-stats")        ; 过渡期按名单取
(engine:metrics :group "jit")
(engine:metrics :prefix "query:")
(require "std/engine-metrics" all:)           ; engine-metrics:get / :group / …
```

`compile:*` 与 `query:*` 中带 `-stats` 后缀的原语大多返回 hash
（非 4 元素 list）。**新代码勿新增 stats 名**（P0b 冻结）；读旧 counter 用 facade。
通过 `hash-ref` 直接按键取：

```scheme
(compile:snapshot)                          ; → hash  8 keys
(hash-ref (compile:snapshot) "marker-user-count") ; → int
(hash-ref (engine:metrics "query:pattern-stats") "schema") ; → int
(hash-ref (dirty:summary) "present-bits")   ; → int (followups)
```

### `(compile:snapshot)` JSON shape — Issue #389

返回当前工作区的观测快照，hash 形态。负载只需 8 个键：

| 键 | 类型 | 含义 |
|----|------|------|
| `marker-user-count` | int | 用户写入的 AST 节点数 (`SyntaxMarker::User`) |
| `marker-macro-introduced-count` | int | 卫生宏插入的节点数 (#247 plumbing) |
| `marker-bool-literal-count` | int | `#t` / `#f` 自动生成节点数 |
| `marker-total-count` | int | marker 列合计 (前 3 项 ≤ 此项) |
| `current-generation` | uint16 | `FlatAST::generation_` (1..65535) |
| `current-wrap-epoch` | uint32 | `wrap_epoch_` 每次 generation 回卷时 +1 |
| `generation-wrap-count` | uint64 | lifetime uint16_t 回卷次数 |
| `node-count` | uint64 | workspace 中总 AST 节点数 (从 `workspace_flat_->size()` 直接读) |

`(query:marker-stats)` 同时提供 4 元素 list 形态 `(user macro bool total)`；
两个原语读同一 underlying 数据，跨形态幂等。深诊断 (narrowing、
typecheck cache、dead-coercion 等) 走每分类 `compile:*-stats`。

实现：`src/compiler/evaluator_primitives_compile.cpp` 中
`add("compile:snapshot", …)`。测试：`tests/test_issue_389.cpp` 5 ACs / 27 检查。

注意：`(stats:get ...)` 在 `lib/std/stats.aura` 里定义为
**字符串名 → primitive 调度器** (走 `(eval name)`)，并非 hash 访问器。
读 snapshot 键请用 `hash-ref`。

## Agent 入口

- JSON：`./build/aura --serve-async` — [wire-formats.md](wire-formats.md)
- 编排：`lib/std/orchestrator.aura` · `tests/suite/orchestrator.aura`

## 标准库

索引：[generated/stdlib-index.md](generated/stdlib-index.md) · 源文件 `lib/std/*.aura`

常用：`(require "std/list" all:)` `(require "std/json" all:)` `(require "std/orchestrator" all:)`

## 开发者

加 primitive：[contributing.md](contributing.md) · 模块地图：[architecture.md](architecture.md)