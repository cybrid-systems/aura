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
| Mutate | `mutate:rebind` `mutate:query-and-replace` `mutate:extract-function` … |
| 验证 | `query:schema` `mutate:validate-against-schema` |
| 版本 | `ast:snapshot` `ast:restore` `rollback` `mutation-history` |
| Workspace | `workspace:create` `workspace:switch` `workspace:merge` `workspace:lock` … |

示例：[tutorial.md](tutorial.md) §EDSL · `tests/suite/mutate-structured.aura`

## Agent 入口

- JSON：`./build/aura --serve-async` — [wire-formats.md](wire-formats.md)
- 编排：`lib/std/orchestrator.aura` · `tests/suite/orchestrator.aura`

## 标准库

索引：[generated/stdlib-index.md](generated/stdlib-index.md) · 源文件 `lib/std/*.aura`

常用：`(require "std/list" all:)` `(require "std/json" all:)` `(require "std/orchestrator" all:)`

## 开发者

加 primitive：[contributing.md](contributing.md) · 模块地图：[architecture.md](architecture.md)