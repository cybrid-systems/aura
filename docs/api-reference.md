# API 表面

**完整原语列表以代码为准**，不要依赖本页的静态枚举。

## 运行时查询

```scheme
(api-reference)    ; REPL 或 eval 中调用 → 全部已注册 primitive 名称
```

实现：`evaluator_primitives_eval.cpp` 中 `primitives_.add("api-reference", …)`，遍历 `Primitives` 槽位生成。

静态扫描（CI 校验）：[generated/primitives.md](generated/primitives.md) — `./build.py docs`

## 自修改 EDSL（摘要）

| 集群 | 代表原语 |
|------|----------|
| 加载 | `set-code` `current-source` `eval-current` `eval-current-output` |
| Query | `query:find` `query:pattern` `query:where` `query:calls` `query:def-use` … |
| Mutate | `mutate:rebind` `mutate:query-and-replace` `mutate:extract-function` `mutate:atomic-batch` … |
| 验证 | `query:schema` `mutate:validate-against-schema` |
| 版本 | `ast:snapshot` `ast:restore` `rollback` `mutation-history` |
| Workspace | `workspace:create` `workspace:switch` `workspace:merge` `workspace:lock` … |

示例：[tutorial.md](tutorial.md) §EDSL · `tests/suite/mutate-structured.aura`

`mutate:atomic-batch` 强原子性、`(atomic-batch:stats)` 可观测字段与并发 fiber 语义见 [design/core/mutate_api.md](design/core/mutate_api.md)。

## 观测原语（hash 形态）

`compile:*` 与 `query:*` 中带 `-stats` 后缀的原语大多返回 hash
（非 4 元素 list）。通过 `hash-ref` 直接按键取：

```scheme
(compile:snapshot)                          ; → hash  8 keys
(hash-ref (compile:snapshot) "marker-user-count") ; → int
(hash-ref (compile:invalidations-stats) "bump-generation-count") ; → int
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