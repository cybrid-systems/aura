# 贡献指南

面向修改 `src/compiler/evaluator.ixx` 及 `evaluator_*.cpp` 分区实现的运行时开发者。

用户/Agent 上手见 [tutorial.md](tutorial.md)；模块地图见 [architecture.md](architecture.md)。

## 构建与测试

```bash
./build.py build
./build.py check          # CI 默认
./build.py test unit      # test_ir
./build.py test integ     # .aura 端到端
```

加 primitive 后至少补 `tests/suite/` 或 `tests/regression/` 用例，并运行 **`./build.py docs`**（会写 `docs/generated/*.md`；CI `./build.py gate` 用 `docs --check` 校验，须把生成文件一并提交）。

## Evaluator 是什么

`Evaluator`（`evaluator.ixx` + 多个 `evaluator_*.cpp` TU）持有 workspace FlatAST、原语表 `Primitives`、执行入口 `eval_flat`，以及 workspace 锁、defuse 失效、快照/回滚等自修改不变式。

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

**加 primitive 前必须先回答** [`docs/design/primitive-vs-stdlib-decision-framework.md`](design/primitive-vs-stdlib-decision-framework.md) **中的问题**：默认应放入 stdlib (`lib/std/`)，只有满足 7 条红线的功能才下沉为 C++ primitive。决定后再回到下面的注册流程。

**P0 已完成**：`init_pair_primitives()` 内无内联 `primitives_.add("...")`；静态原语均在 `evaluator_primitives_*.cpp`（31 个 TU），经 `prim_registrar()` 回调注册。完整列表见 `docs/generated/primitives.md`（`./build.py docs` 生成）。

注册点：`init_pair_primitives()`、`Evaluator()` 构造器（network/type 等），或 `ffi_runtime_` / `adt_runtime_`（外部 runtime 模式）。

| 簇 / 前缀 | 源文件 | 注册函数 | 备注 |
|-----------|--------|----------|------|
| 类型谓词 / `not` | `evaluator_primitives_core.cpp` | `register_type_and_char_primitives` | 无状态 |
| pair / string | `evaluator_primitives_pair.cpp` | `register_pair_and_string_primitives` | `pairs_` / `string_heap_` |
| list 高阶 | `evaluator_primitives_list.cpp` | `register_list_primitives` | 收 `Evaluator&`；`apply_unary/pred/binary` 在 list TU |
| JSON | `evaluator_primitives_json.cpp` | `register_json_primitives` | |
| vector / hash | `evaluator_primitives_vector.cpp` | `register_vector_and_hash_primitives` | + `vector_heap_` |
| math / regex | `evaluator_primitives_math.cpp` | `register_math_regex_and_arithmetic_primitives` | |
| reflect / keyword | `evaluator_primitives_reflect.cpp` | `register_reflect_and_type_primitives` | + `keyword_table_` / `type_registry_` |
| `query:module-*` | `evaluator_primitives_query.cpp` | `register_query_primitives` | + `resolve_module_path` |
| `query:*` workspace | `evaluator_primitives_query_workspace.cpp` | `register_workspace_query_primitives` | + `mev` |
| `query:*` def-use | `evaluator_primitives_query_defuse.cpp` | `register_defuse_query_primitives` | + `DefUseQueryCallbacks` |
| `mutate:*` | `evaluator_primitives_mutate.cpp` | `register_mutate_primitives` | friend + `mev` + `destroy_defuse_index` |
| `workspace:*` | `evaluator_primitives_workspace.cpp` | `register_workspace_primitives` | `WorkspaceTree` 在 `evaluator.ixx` |
| `ast:*` | `evaluator_primitives_ast.cpp` | `register_ast_primitives` | + `defuse_summary_stats` |
| `compile:*` / JIT 观测 | `evaluator_primitives_compile.cpp` / `observability.cpp` | 各 `register_*` | friend |
| messaging / fiber | `evaluator_primitives_messaging.cpp` | `register_messaging_primitives` | |
| git / network | `evaluator_primitives_io.cpp` | `register_git/network_primitives` | |
| agent / synthesize | `evaluator_primitives_agent.cpp` | `register_auto_evolve/synthesize/strategy` | |
| memory / policy / eval EDSL | `memory.cpp` / `policy.cpp` / `eval.cpp` | 各 `register_*` | |
| types / diagnostic / module / file / runtime | `types` … `runtime.cpp` 等 | 各 `register_*` | P0 step 22–31 |

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

`aura.compiler.evaluator` 由 `evaluator.ixx`（模块接口）+ 44 个 `.cpp` 分区 TU 组成；原语簇与 `register_*` 对应关系见 §2 表。

### 接口 + 13 核心 `.cpp`

| 文件 | 职责 |
|------|------|
| `evaluator.ixx` | 模块接口；`primitives_detail::register_*` 前向声明 hub |
| `evaluator_ctor.cpp` | `Evaluator` 构造/析构；构造器内原语（memory/messaging/policy/types） |
| `evaluator_eval_flat.cpp` | `apply_closure`、`eval_flat`、宏展开、`post_mutation_macro_reexpand`、require/functor |
| `evaluator_env.cpp` | `Env` / `EnvFrame` / `EnvView`、SoA arena、`bind_symid` |
| `evaluator_module_loader.cpp` | `resolve_module_path`、`load_module_file`、`gc_module` |
| `evaluator_workspace_tree.cpp` | workspace tree、panic checkpoint、`copy_env` |
| `evaluator_defuse_index.cpp` | `DefUseIndex`、`defuse_index_destroy`、`install_defuse_subsystem` |
| `evaluator_gc.cpp` | `flush_gc_roots`、sweep、`compact_pairs` |
| `evaluator_typecheck.cpp` | `run_typecheck_no_lock*` |
| `evaluator_adt.cpp` | 动态 ADT ctor 注册、`make_merr` |
| `evaluator_fiber_mutation.cpp` | per-fiber mutation stack、`yield_mutation_boundary` |
| `evaluator_query_index.cpp` | `build_tag_arity_index`（`query:pattern` 加速） |
| `evaluator_primitives_registry.cpp` | `register_all_primitives` 编排（`init_pair_primitives`） |
| `evaluator_primitives_builtins.cpp` | `Primitives` 内置算术/布尔（`+` `-` `*` `/` 等） |

### 原语分区（31 TU，经 registry 或 ctor 注册）

| 文件 | 簇 / 前缀 |
|------|-----------|
| `evaluator_primitives_core.cpp` | 类型谓词、`not` |
| `evaluator_primitives_pair.cpp` | pair / string heap |
| `evaluator_primitives_list.cpp` | list 高阶、`drop` |
| `evaluator_primitives_json.cpp` | JSON encode/parse |
| `evaluator_primitives_vector.cpp` | vector / hash |
| `evaluator_primitives_math.cpp` | m4-linear、regex、math |
| `evaluator_primitives_reflect.cpp` | reflect / type / keyword |
| `evaluator_primitives_query.cpp` | `query:module-*` |
| `evaluator_primitives_query_workspace.cpp` | workspace AST `query:*` |
| `evaluator_primitives_query_defuse.cpp` | def-use `query:*` |
| `evaluator_primitives_mutate.cpp` | `mutate:*` |
| `evaluator_primitives_workspace.cpp` | `workspace:*` |
| `evaluator_primitives_ast.cpp` | `ast:*` |
| `evaluator_primitives_compile.cpp` | `compile:*`、concurrency、syntax-marker |
| `evaluator_primitives_observability.cpp` | panic / stats / jit / gc-arena |
| `evaluator_primitives_messaging.cpp` | messaging / fiber / channel（ctor） |
| `evaluator_primitives_io.cpp` | git:*、network |
| `evaluator_primitives_agent.cpp` | auto-evolve / synthesize / strategy |
| `evaluator_primitives_memory.cpp` | coverage / gc / arena / dirty（ctor） |
| `evaluator_primitives_policy.cpp` | memory-policy / capability（ctor） |
| `evaluator_primitives_eval.cpp` | `set-code` / `eval-current` / `api-reference` |
| `evaluator_primitives_types.cpp` | declare-type / hot-swap（ctor） |
| `evaluator_primitives_diagnostic.cpp` | diagnose / apply-fix |
| `evaluator_primitives_module.cpp` | module? / use / load-module |
| `evaluator_primitives_file.cpp` | read / write-file / shell |
| `evaluator_primitives_runtime.cpp` | equal? / display / format / `io_print_val` |
| `evaluator_primitives_test.cpp` | `run-tests` |
| `evaluator_primitives_misc.cpp` | current-time / arena-offset |
| `evaluator_primitives_control.cpp` | `while` |
| `evaluator_primitives_char.cpp` | char? / string->list / read-line |
| `evaluator_primitives_mutation.cpp` | mutation-count / rollback |

### 相邻模块

| 文件 | 职责 |
|------|------|
| `evaluator_pure.ixx` | 无 `Evaluator` 状态的纯函数（`coerce_value_pure` 等） |
| `query.ixx` / `query_impl.cpp` | QueryEngine、ASTIndex |
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
## Sanitizers（Issue #299）

`build.py` 支持 AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer 三种插桩构建。三者路由到不同 build 目录，互不污染普通 `build/`。

```bash
# 本地 ASAN — 内存安全（UAF、double-free、OOB、leaks）
./build.py --sanitizer=asan build
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
  ./build_asan/aura < tests/multi_session_leak_test.aura
./build.py --sanitizer=asan test unit     # test_ir 在 ASAN 下跑

# 本地 UBSAN — UB 检测（有符号溢出、shift OOB、null deref、type confusion）
./build.py --sanitizer=ubsan build
./build.py --sanitizer=ubsan test unit

# 本地 TSAN — 数据竞争（fiber + 并发 mutate/query）
./build.py --sanitizer=tsan build
./build.py --sanitizer=tsan test concurrent
```

或者直接走 CMakePresets（与 build.py --sanitizer 行为完全一致）：

```bash
cmake --preset asan   && cmake --build --preset asan
cmake --preset ubsan  && cmake --build --preset ubsan
cmake --preset tsan   && cmake --build --preset tsan
```

### 限制

- **TSan 与 ASan 不兼容**。同时需要两者得跑两次（先 asan 后 tsan），不能合并。
- **TSan 强制 `CMAKE_BUILD_TYPE=Debug`**。`-O2/-O3` 下 TSan 大量误报，由 build.py 自动覆盖。
- **ASan 内存增长 ~2x、速度 ~2x slow**。CI 上默认只跑 `asan-build` + `asan-verify` 两个 job，PR 不跑。
- **ASan 与 LLVM JIT** 一起开可能 false positive 报告 `use-after-scope` 在 ORC 内部分配器。`--sanitizer=asan` 默认不启用 `AURA_HAVE_LLVM=1`，需要完整覆盖时手动 `cmake -DAURA_HAVE_LLVM=1 -B build_asan`。
- 跑 TSan 必须设置 `TSAN_OPTIONS=halt_on_error=1`，否则报错只 warn 不退出。

## §X Coercion elision guidelines (Issue #508)

`DeadCoercionEliminationPass`（`pass_manager.ixx`）在 lowering 之后、IR 解释器/JIT 之前运行。它替换掉多余的 `CastOp`，是 gradual typing + typed mutation 零开销路径的关键。

**三条规则**：

1. **identity cast**（source.type_id == target.type_id）→ 替换为 `Local`。
2. **nested cast**（`(cast (cast x T1) T2)`）→ 跳过中间 cast，直接 `(cast x T2)`，然后规则 1 继续消除。
3. **safe Dynamic passthrough**（target.type_tag ≥ 3，源 slot 有 ground type_id）→ 替换为 `Local`。
   IR 解释器对 `type_tag ≥ 3` 的 default case 就是 `locals[ops[0]] = val`，纯 passthrough。条件是源 slot 必须是 ground-typed（type_id ≠ 0）—— 没 type 信息的 source 不能 elide，因为 lowering 可能为某种语义边界专门插了 CastOp。

**保守原则**：

- **没 type info 不动**：源 slot `type_id == 0` 时，规则 3 不触发。这避免了 elide 掉跨模块边界 / FFI 入口处的 CastOp。
- **类型不匹配不动**：规则 1 要求 source 和 target type_id 都非零且相等。
- **配置开关**：`DeadCoercionEliminationPass::set_keep_for_debug(true)` 让 pass 变成 no-op，但 `kept_for_debug_count()` 告诉你"假如没开 debug 会 elide 多少"。用于 blame 模式调试。

**观测**：

- `(compile:dead-coercion-stats)` — lifetime 已消除 CastOp 计数（自程序启动累计）。
- `(compile:dead-coercion-elapsed)` — lifetime 在 pass 里花掉的微秒数。
- `(compile:dead-coercion-kept-for-debug)` — lifetime 在 keep_for_debug 模式下"本会消除"的 CastOp 计数。
- `CompilerService::snapshot().dead_coercion_*` 同样暴露这三个字段。

**添加新规则时的检查清单**：

- [ ] 在 `tests/test_issue_508.cpp` 加一个 AC（identity / nested / Dynamic passthrough / keep_for_debug / 时间 / 端到端 gradual）。
- [ ] 若规则依赖 type_reg 信息，在 `tests/test_ir.cpp` 对应位置补一个 `(reg2)` 重载测试。
- [ ] 更新 `docs/contributing.md` 本节，描述新规则的语义边界。
- [ ] 跑 `./build.py check`（test_ir + test_issue_433 + test_issue_508 全绿）。
