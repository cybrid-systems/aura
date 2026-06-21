# 贡献指南

面向修改 `src/compiler/evaluator_impl.cpp` / `evaluator.ixx` 的运行时开发者。

用户/Agent 上手见 [tutorial.md](tutorial.md)；模块地图见 [architecture.md](architecture.md)。

## 构建与测试

```bash
./build.py build
./build.py check          # CI 默认
./build.py test unit      # test_ir
./build.py test integ     # .aura 端到端
```

加 primitive 后至少补 `tests/suite/` 或 `tests/regression/` 用例，并运行 `./build.py docs` 更新 `docs/generated/primitives.md`。

## Evaluator 是什么

`Evaluator`（`evaluator.ixx` + `evaluator_impl.cpp`）持有 workspace FlatAST、原语表 `Primitives`、执行入口 `eval_flat`，以及 workspace 锁、defuse 失效、快照/回滚等自修改不变式。

IR 解释器与 JIT 是下游，语义以 `eval_flat` 为准。

## 三个不变式

1. **Flat 在求值中可增长** — `parse_to_flat`、`add_node` 等会追加节点；遍历时可能正在增长。
2. **query 与 mutate 可并发** — REPL、`--serve`、多 fiber 共享实例；`workspace_mtx_` 是唯一边界。
3. **节点修改使 def-use 失效** — 必须走 `defuse_touch_fn_` / `defuse_affected_syms_` 协议。

## §1 自修改 Flat 迭代铁律（Issue #111）

最大 footgun：循环条件每次读 `flat.size()`，循环体又可能增长 flat → 无限循环 / OOM。

```cpp
// 错误
for (aura::ast::NodeId id = 0; id < flat.size(); ++id) { ... }

// 正确
const auto end_id = flat.size();
for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }
```

同时满足才必须 snapshot：(1) 遍历 FlatAST；(2) 条件重读 `.size()`；(3) 循环体可能追加节点。拿不准就 snapshot。

## §2 添加 primitive

注册点：`init_pair_primitives()` 或 `Evaluator()` 构造器（需 `[this]` 时）。
无状态原语可放入 `evaluator_primitives_*.cpp`（P0：`evaluator_primitives_core.cpp`），经 `PrimRegistrar` 回调注册。需访问 `pairs_` / `string_heap_` 的见 `evaluator_primitives_pair.cpp`；list 高阶原语（`map` / `filter` / `foldl`）见 `evaluator_primitives_list.cpp`（额外传入 `apply_unary` / `apply_pred` / `apply_binary` 回调）；JSON 见 `evaluator_primitives_json.cpp`；vector/hash 见 `evaluator_primitives_vector.cpp`（额外传入 `vector_heap_`）；m4-linear / regex / math / arithmetic 见 `evaluator_primitives_math.cpp`；reflect / type / keyword 见 `evaluator_primitives_reflect.cpp`（额外传入 `keyword_table_` 与 `type_registry_`）；独立 `query:` 原语（`query:module-exports`、`query:schema`）见 `evaluator_primitives_query.cpp`（额外传入 `resolve_module_path` 回调）；workspace AST `query:*` 簇见 `evaluator_primitives_query_workspace.cpp`（`WorkspaceQueryState` + `mev` 回调）；def-use 索引 `query:*` 簇见 `evaluator_primitives_query_defuse.cpp`（`DefUseQueryCallbacks` + `make_merr`）；`mutate:*` 簇见 `evaluator_primitives_mutate.cpp`（`Evaluator&` + `mev` + `destroy_defuse_index` 回调）；`workspace:*` 簇见 `evaluator_primitives_workspace.cpp`（`Evaluator&` friend + `destroy_defuse_index`；`WorkspaceTree` 类型在 `evaluator.ixx`）；`ast:*` 簇见 `evaluator_primitives_ast.cpp`（`Evaluator&` friend + `destroy_defuse_index` + `defuse_summary_stats` 回调）；`compile:*` / `concurrency:*` / `syntax-marker` 簇见 `evaluator_primitives_compile.cpp`（`Evaluator&` friend）；panic / `typecheck-status` / `atomic-batch:stats` / `closure:stats` 与 `jit:*` / `gc-arena-*` 观测原语见 `evaluator_primitives_observability.cpp`（`register_eval_observability_primitives` + `register_jit_arena_primitives`，`Evaluator&` friend）；messaging / `fiber:*` / `channel:*` / `orch:*` 簇见 `evaluator_primitives_messaging.cpp`（`Evaluator&` friend；`FiberResult` 静态状态留在该 TU）；`git:*` 与 `getenv` / `http-*` / `tcp-*` 见 `evaluator_primitives_io.cpp`（`register_git_primitives` + `register_network_primitives`；`CurlAPI` dlopen 留在该 TU）；`auto-evolve:*` / `synthesize:*` / `intend` / `strategy-*` 见 `evaluator_primitives_agent.cpp`（`register_auto_evolve_primitives` + `register_synthesize_primitives` + `register_strategy_primitives`；模板静态存储留在该 TU）；`coverage-report` / `gc*` / `arena:*` / `string-pool:*` / `dirty:*` / `type-registry-*` / `memory-pressure` 见 `evaluator_primitives_memory.cpp`（`register_memory_primitives` + `destroy_defuse_index` 回调）。

```cpp
primitives_.add("my:primitive", [](std::span<const EvalValue> a) -> EvalValue {
    if (a.size() < 1 || !types::is_int(a[0]))
        return make_void();
    return make_int(types::as_int(a[0]) * 2);
});
```

要点：
- 参数类型固定为 `std::span<const EvalValue>`，先验 `a.size()` 与各 `is_*`
- 返回 `EvalValue`，用户错误用值通道（`make_merr` / `#f`），不要 throw
- 构造器：`make_int` / `make_string` / `make_void` 等见 `value.ixx`

## §3 Mutate 锁协议

`workspace_mtx_` 是 `std::shared_mutex`：

| 种类 | 锁 |
|------|-----|
| 写 workspace AST 的 `mutate:*` | `std::unique_lock` |
| 只读 `query:*` | `std::shared_lock` |

### 标准 mutate 骨架

```cpp
primitives_.add("mutate:foo", [this](std::span<const EvalValue> a) -> EvalValue {
    if (workspace_read_only_) return make_merr("read-only", "...");
    std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);
    defuse_version_++;
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();
    if (!workspace_flat_) return make_merr("no-workspace", "...");
    auto& flat = *workspace_flat_;
    // ... validate, mutate (§1 snapshot if iterating) ...
    flat.add_mutation_with_rollback(...);
    workspace_flat_->mark_dirty_upward(node);
    if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);
    defuse_affected_syms_.insert(name);
    return make_int(mid);
});
```

错误返回统一用成员 `make_merr(kind, msg)`，不要新建局部 `merr` lambda。

**死锁**：持 `unique_lock` 时勿调用 `ensure_defuse`、`apply_closure`、`typecheck-current`（它们会再抢锁）。先释放锁或拆成两阶段。

## §4 DefUseIndex touch

修改节点后必须：

```cpp
defuse_affected_syms_.insert(name);
if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);
```

`defuse_touch_fn_` 为 null 时仍安全；下次 `ensure_defuse` 会全量重建。

## 合并前检查清单

- [ ] §1：可增长 flat 的遍历已 snapshot `end_id`
- [ ] §3：mutate 用 `unique_lock`，query 用 `shared_lock`，无重入锁
- [ ] §4：defuse touch 双路径
- [ ] 参数校验在 `as_*` / 索引访问之前
- [ ] `workspace_read_only_` 快速路径
- [ ] mutation boundary yield
- [ ] `add_mutation_with_rollback` + `mark_dirty_upward`
- [ ] fuzz：`fuzz_defuse.py --quick`、`fuzz_workspace.py --quick`、`fuzz_snapshot.py --quick`
- [ ] ASAN：`ASAN_OPTIONS=detect_leaks=1 ./build/test_ir`

## 文件地图

| 文件 | 职责 |
|------|------|
| `evaluator.ixx` / `evaluator_impl.cpp` | Evaluator、原语、eval_flat、EDSL |
| `query.ixx` / `query_impl.cpp` | QueryEngine、DefUseIndex |
| `adt_runtime.*` | ADT 构造器表（Issue #108 bypass） |
| `ffi_primitives.*` | C FFI 状态 |
| `service.ixx` | CompilerService、增量 cache |
| `ir_executor.*` | IR 解释器 |
| `aura_jit.*` | LLVM JIT |
| `src/serve/*` | fiber、scheduler、serve-async |
| `main.cpp` | CLI、`--serve` JSON 分发 |

## 维护规则（3 条）

1. **加/改 primitive** → 测试用例 + `./build.py docs`（`check` 会校验未过期）。
2. **改 serve 协议** → [wire-formats.md](wire-formats.md) + `tests/test_serve_async.aura`。
3. **改不变式** → 更新本文 §1–§4 或 [architecture.md](architecture.md)。

历史设计：`git tag docs-archive-pre-2026-06`。