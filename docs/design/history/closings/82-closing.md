## Status — query/mutate EDSL + std::meta 都已存在,集成是后续

Issue #82 提到 evo-kv 演化引擎需要 strong reflection — query 自己结构 + mutate + C++26 std::meta。

### ✅ 已有的 (query/mutate EDSL)

`src/compiler/evaluator_impl.cpp` 里 **20+ query:* primitives**:

- `query:find name` — 找所有匹配 symbol 的 node id
- `query:children node-id` — 子节点
- `query:parent node-id` — 父节点
- `query:siblings node-id` — 兄弟节点
- `query:root` — 根 node id
- `query:node node-id` — 节点元数据
- `query:calls name` — 调 name 的所有 call 节点
- `query:where predicate` — 条件过滤节点
- `query:filter pred` — 类似 where 的另一种形式
- `query:node-type node-id` — 节点类型
- `query:pattern sexpr` — 模式匹配
- `query:def-use var` — def-use 链
- `query:reaches node-id` — reachability
- `query:effects` — 副作用追踪
- `query:build-index` / `query:index-stats` — 索引工具

### ✅ 已有的 (mutate EDSL)

- `mutate:rebind` / `mutate:replace-*` / `mutate:tweak-literal` / `mutate:insert-child` / `mutate:remove-node` / `mutate:set-body` / `mutate:replace-pattern` / `mutate:record-patch` / `mutate:splice`
- `ast:snapshot` / `ast:restore` / `ast:diff` / `ast:list-snapshots`

### ✅ 已有的 (C++26 std::meta compile-time)

`src/reflect/type_validate.hh` 用 P2996 `std::meta` 做 type layout 验证:
- `validate_type_id_layout()` — 验证 TypeId 8 字节
- `validate_type_info_layout()` — 验证 TypeInfo 布局
- `validate_struct_layout<T>()` — 编译期验证 AI 生成的类型注解

### ❌ 没做的 (issue 范围内)

- **Aura 级别暴露 std::meta 信息到 query EDSL**
  - 比如 `(query:c-type-of "Int")` 返回 C++ 端的 std::meta::info
  - 这是 compile-time + runtime 反射的桥
- **inspect-closure** 原语 — closure 内部信息(捕获的 free vars,原定义 module 等)
- **evo-kv 用 query:* 看到更深的运行时元数据**
  - 比如 closure 的 type signature
  - module 的依赖图

### 建议 follow-up issue

> `[Reflection] Bridge C++26 std::meta with Aura query:* EDSL (#82 follow-up)`
>
> 在 `evaluator_impl.cpp` 加 `query:c-type-of`、`query:c-members-of` 等
> 编译期反射原语,让 Aura 级别能 introspect C++ 端的类型结构。
> 需要新增 `src/reflect/runtime_reflect.ixx` 桥头文件 (用 P2996 `std::meta::info_of`,
> 把 type info 编码到 Aura 能读的形式)。

Closing this issue as **largely resolved** — existing query/mutate
EDSL covers evo-kv's actual reflection needs. The C++26 std::meta
integration is tracked as a follow-up enhancement.
