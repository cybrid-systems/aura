# 贡献指南

面向修改 `src/compiler/evaluator.ixx` 及 `evaluator_*.cpp` 分区实现的运行时开发者。

模块地图见 [architecture.md](architecture.md)。架构 / protocol 改动同步 [wire-formats.md](wire-formats.md)。

## 构建与测试

```bash
./scripts/install-githooks.sh   # 一次性：启用 pre-commit hook
./build.py build
./build.py gate              # CI gate(静态 + format + fixtures + surface)
./build.py test unit         # test_ir
./build.py test integ        # .aura 端到端
./build.py bench --strict    # #1569：SLO 硬门栅
```

加 primitive 后：`./build.py docs` 自动 regen `docs/generated/primitives.md`。

## 三个不变式

1. **Flat 在求值中可增长** — 遍历时若循环体可能追加节点,先 snapshot `end_id = flat.size()`。
2. **query 与 mutate 可并发** — `workspace_mtx_` 是唯一边界(`unique_lock` 写 / `shared_lock` 读)。
3. **节点修改使 def-use 失效** — 必须走 `defuse_touch_fn_` / `defuse_affected_syms_` 协议。

## Mutate 骨架

```cpp
primitives_.add("mutate:foo", [this](std::span<const EvalValue> a) -> EvalValue {
    if (workspace_read_only_) return make_merr("read-only", "...");
    std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);
    defuse_version_++;
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();
    if (!workspace_flat_) return make_merr("no-workspace", "...");
    auto& flat = *workspace_flat_;
    // ... validate, mutate (snapshot §1 if iterating) ...
    flat.add_mutation_with_rollback(...);
    workspace_flat_->mark_dirty_upward(node);
    if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);
    defuse_affected_syms_.insert(name);
    return make_int(mid);
});
```

持 `unique_lock` 时**不要**调 `ensure_defuse` / `apply_closure` / `typecheck-current`(会再抢锁,死锁)。

## 注册 primitive

```cpp
primitives_.add("my:primitive", [&ev](std::span<const EvalValue> a) -> EvalValue {
    if (a.size() != 1 || !types::is_int(a[0])) return make_void();
    return make_int(types::as_int(a[0]) * 2);
}, PrimMeta{.arity = 1, .pure = true, .safety_flags = 0,
             .doc = "Double an integer argument."});
```

要点：
- 先验 `a.size()` 与各 `is_*`
- 用户错误走值通道(`make_void` / `make_merr`),不 throw
- 修 primitive 后 `./build.py docs`(自动 regen `docs/generated/primitives.md`)
- `add_mutate` 自动套 `MutationBoundaryGuard`(优先于 `add`)

## Discoverability (Issue #1552)

| 表面 | 用法 |
|------|------|
| Stdlib facade | `(require "std/primitives" all:)` → `primitives:help` / `primitives:list` / `primitives:discover` |
| Runtime meta | `(primitive:describe name)` / `(query:primitives-meta-catalog)` |
| 生成文档 | `docs/generated/primitives.md` |

## 合并前检查清单

- [ ] §1：可增长 flat 的遍历已 snapshot `end_id`
- [ ] mutate 用 `unique_lock`,query 用 `shared_lock`,无重入锁
- [ ] defuse touch 双路径(`defuse_affected_syms_.insert` + `defuse_touch_fn_`)
- [ ] 参数校验在 `as_*` / 索引访问之前
- [ ] mutation boundary yield(if Fiber-aware)
- [ ] `add_mutation_with_rollback` + `mark_dirty_upward`
- [ ] 至少补 `tests/` 用例(unit 或 suite)
- [ ] `./build.py gate` 全绿(包括 docs regen)

## 文件地图

`aura.compiler.evaluator` 由 `evaluator.ixx` 接口 + 44 个 `.cpp` 分区 TU 组成:

| 文件 | 职责 |
|------|------|
| `evaluator.ixx` | 模块接口 / `primitives_detail::register_*` 前向声明 hub |
| `evaluator_ctor.cpp` | `Evaluator` 构造 / 析构 / 构造器内原语 |
| `evaluator_eval_flat.cpp` | `apply_closure` / `eval_flat` / 宏 / require / functor |
| `evaluator_env.cpp` | `Env` / `EnvFrame` / SoA arena / `bind_symid` |
| `evaluator_workspace_tree.cpp` | workspace tree / panic checkpoint |
| `evaluator_defuse_index.cpp` | `DefUseIndex` / `install_defuse_subsystem` |
| `evaluator_gc.cpp` | GC roots / sweep |
| `evaluator_fiber_mutation.cpp` | per-fiber mutation stack / `yield_mutation_boundary` |
| `evaluator_primitives_registry.cpp` | `register_all_primitives` 编排 |
| `evaluator_primitives_builtins.cpp` | 算术 / 布尔内置 |
| `evaluator_primitives_*.cpp` | 31 个原语 TU(每簇一个,经 registry 注册) |

相邻模块:`evaluator_pure.ixx` / `query.ixx` / `service.ixx` / `ir_executor.*` / `aura_jit.*` / `src/serve/*` / `src/orch/*` / `main.cpp`。