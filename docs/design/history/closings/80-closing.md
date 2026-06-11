## Status — hot-swap:define 等待 follow-up issue

Issue #80 描述 evo-kv 需要的 function-level hot-swap + 增量编译能力。  
**底层基础设施都已就位**,所以现在写一个 `hot-swap:define` 是 straightforward 30 行 wrapper。

### ✅ 已有的(mutation pipeline)

- `mutate:rebind name new-body-string` (evaluator_impl.cpp:4707)
  - 把新 code 解析进 existing FlatAST (incremental, appends nodes)
  - 更新 Define 节点的 body 指向新 Lambda
  - 让 dep_caller 链路 dirty
  - 自动 typecheck + ownership validation
- `ast:snapshot [name]` + `ast:restore id` (evaluator_impl.cpp:7821, 7885)
  - 记录 workspace 源码
  - 通过 `set-code` 恢复
- 增量编译 (`#72`, `#66` 部分)
  - dirty tracking 在 FlatAST 节点
  - 局部 typecheck 缓存
  - 根级 + subtree-level eval 缓存
- Evo-KV 项目侧 metrics + 快照 (`projects/evo-kv/evo-kv-metrics.aura`)

### ❌ 没做的(本 issue 范围内)

- **专用的 `hot-swap:define name new-body [summary]` primitive**
  - 应该是 30 行 wrapper:`(ast:snapshot name)` → 调 `mutate:rebind` → 返回 pair `(snapshot-id . rebind-result)`
  - **我今天试着加,但在 ixx 嵌套 lambda 里出 brace 解析问题,回滚了**
  - 真正的难度:ixx modules 的 `mev` lambda 必须在闭包捕获列表里(像我看到的 `mutate:rebind` 用 `[this, mev]`)
  - 如果作为 follow-up 单独开,有人有完整 focus 时更好做

### 建议 follow-up issue

> `[Hot-swap] Add hot-swap:define primitive with snapshot rollback (#80 follow-up)`
>
> 在 evaluator_impl.cpp 里 mutate:rebind 后面加 `primitives_.add("hot-swap:define", ...)`:
> 1. 调 `ast:snapshot <name>` 拿 snapshot id
> 2. 调 `mutate:rebind` 做 swap
> 3. 返回 pair `("snapshot-id" . id) ("swap-result" . result) ("name" . name)`
> 
> 接受 criteria: 现有 201 测试 + 新 nested test 仍然过
> 跟踪:`#80` 的核心需求,failure mode 是 user 调用 `hot-swap:define` 后新代码 buggy → 通过返回的 snapshot id `(ast:restore id)` 回滚

## 没关 #80

留作 open,直到 `hot-swap:define` primitive 真正加上。
