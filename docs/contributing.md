# 贡献指南

面向修改 `src/compiler/evaluator.ixx` 及 `evaluator_*.cpp` 分区实现的运行时开发者。

用户/Agent 上手见 [tutorial.md](tutorial.md)；模块地图见 [architecture.md](architecture.md)。

## 构建与测试

```bash
./scripts/install-githooks.sh   # 一次性：启用 pre-commit（含 docs 自动 regen）
./build.py build
./build.py check          # CI 默认
./build.py test unit      # test_ir
./build.py test integ     # .aura 端到端
```

**Git hooks**：`.githooks/pre-commit` 已在仓库内，但 Git **不会自动启用**——须运行 `./scripts/install-githooks.sh`（设置 `core.hooksPath=.githooks`）。启用后，staged `src/` 变更会在 commit 前自动跑 `./build.py docs` 并 re-stage `docs/generated/*.md`；CI `./build.py gate` 仍作最终校验。

加 primitive 后至少补 `tests/suite/` 或 `tests/regression/` 用例。若未装 hook，须手动 **`./build.py docs`** 并把 `docs/generated/*.md` 一并提交（否则 `docs --check` 挂）。

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

### AI Agent Primitive Development Kit（Issue #480）

注册时可附带 `PrimMeta`（`arity` / `pure` / `safety_flags` / `doc`），Agent 通过 `(primitive:describe name)` 与 `(query:primitive-list-with-meta)` 自省元数据。

```cpp
// 模板：lambda 骨架 + 参数校验 + 带 meta 注册
primitives_.add(
    "my:primitive",
    [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() != 1 || !types::is_int(a[0]))
            return make_void();
        return make_int(types::as_int(a[0]) * 2);
    },
    PrimMeta{.arity = 1,
             .pure = true,
             .safety_flags = 0,
             .doc = "Double an integer argument."});
```

`safety_flags`：`0x01` 修改 workspace、`0x02` IO 副作用、`0x04` fiber 敏感。`arity = 255` 表示可变参数。观测：`query:primitive-meta-stats`。

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

## 生产构建 / 可复现发布（Issue #675）

CI 在 PR 上强制 **UBSAN smoke** + **security-scan**；`main` 额外跑 **reproducible-build** 双构建校验。Nightly workflow 跑扩展 fuzz + TSAN。

```bash
# 可复现 Release 构建（SOURCE_DATE_EPOCH + prefix map + 固定 random seed）
SOURCE_DATE_EPOCH=1704067200 ./build.py repro build

# 双构建字节级校验（strip 后 SHA-256 必须一致）
./build.py repro --verify

# CycloneDX SBOM
./build.py sbom --version=1.0.0

# 漏洞扫描（优先 Trivy，回退 pip-audit）
./build.py security

# 完整发布包（tarball + SBOM）
SOURCE_DATE_EPOCH=1704067200 ./scripts/release.sh 1.0.0
```

**观测**：`(query:ci-reproducibility-stats)` 返回 5 字段 hash（`source-date-epoch`、`build-type`、`sanitizer-mode`、`reproducible-flags-active`、`ccache-disabled`）。

**本地对齐 CI**：`CCACHE_DISABLE=1 ./build.py check`；需要 sanitizer 矩阵时加 `./build.py --sanitizer=ubsan test unit`。

## §X Incremental ConstraintSystem soundness (Issue #432 / #466)

`ConstraintSystem::solve_delta()`（`type_checker_impl.cpp`）是 `infer_flat_partial` 多节点增量推断的快路径：只处理 `add_delta` 标记的 dirty 约束子集。

**跨 delta 冲突检测（#466）**：

1. `unify` / `consistent_unify` 在 delta 求解期间通过 `note_touched_var` 记录被重绑的 Union-Find 根（`touched_roots_`）。
2. dirty worklist 收敛后，`reverify_clean_constraints_for_touched()` 对引用这些根的 **clean** 约束做有界重放（上限 `kReverifyCleanScanLimit = 256`）。
3. 重放失败 → `SolveResult::CONFLICT`（无需 fallback 到全量 `solve()`）。

**观测**：

- `(query:constraint-stats)` — `delta_conflict_reverify_total + delta_conflict_detected_total` 之和。
- `(query:constraint-delta-stats)` — `touched_roots_hits`（`delta_conflict_reverify_total`）+ `cross_delta_conflicts_caught` 之和。
- `CompilerService::snapshot().delta_conflict_*` — lifetime 计数器。

**回归测试**：`tests/test_incremental_type_soundness.cpp`（16 AC，含注入冲突矩阵 ≥50% 检出率 + 多轮 mutate smoke）。

**修改 solve_delta 时的检查清单**：

- [ ] 保持 touched-root 跟踪在 `unify` / `consistent_unify` 路径上完整。
- [ ] 新冲突模式需在 `test_incremental_type_soundness.cpp` 加合成用例。
- [ ] 有界扫描上限变更时更新 `kReverifyCleanScanLimit` 注释与测试预期。
- [ ] 跑 `./build.py check` 或至少 `test_incremental_type_soundness`。

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

## §X Runtime.c 查找策略（Issue #237 v4 / #360 / #374）

AOT 路径需要一个能运行的 `lib/runtime.c` 源文件。`find_runtime_c()`（`src/compiler/aura_jit_bridge.cpp`）按以下顺序试候选路径，返回第一个能 `fopen` 成功的：

1. **`$AURA_RUNTIME_DIR/runtime.c`** — 环境变量显式覆盖，最高优先级。
2. **从 aura binary 所在目录向上 walk 8 层**，每层试 `lib/runtime.c`。覆盖：
   - dev-machine layout：`build/aura` + 相对源码 `lib/runtime.c`
   - CI build tree：`build/aura` + 多个 `..` 后的 `lib/runtime.c`
   - 典型 install layout：`/usr/local/bin/aura` + `../lib/runtime.c`（走 walk-up 也行）
3. **CWD 相对 fallback**：`lib/runtime.c`、`../lib/runtime.c`。保留是为了 pre-#237 启动脚本。
4. **Install path fallback（Issue #360）**：`/usr/local/share/aura/runtime.c`、`/usr/share/aura/runtime.c`、`/opt/aura/share/runtime.c`。FHS-compliant，`make install` 后开箱即用，无需设 `AURA_RUNTIME_DIR`。

**CI 用法**：在 `.github/workflows/ci.yml` 的 `build-test` job 里，如果 `runtime.c` 在某条路径找不到（容器 `/proc` 受限、build tree 路径不在 walk-up 8 层内），可显式设 `AURA_RUNTIME_DIR=$GITHUB_WORKSPACE` 强制指向仓库根（运行时通过相对路径在 `$AURA_RUNTIME_DIR/lib/runtime.c` 找到）。Pre-#237 时代是 `1 passed + 4 failed`，post-#237 v4 是 `11/11`，post-#360 加 install-path fallback 让本地 `make install` 也跑得通。

**调试验证**：如果 `aura --emit-binary` 报 `AOT: cannot find runtime.c`，按以下顺序排查：

- `echo $AURA_RUNTIME_DIR` — 是否设了，路径里有没有 `runtime.c`。
- `ls -l $(which aura)` 所在目录 → 上 8 层，每层试 `ls lib/runtime.c`。
- 走 install path 时 `ls /usr/local/share/aura/runtime.c` / `ls /usr/share/aura/runtime.c` / `ls /opt/aura/share/runtime.c`。
- 找到路径后 `head -3 <path>` 确认是 C 源码不是空文件或 binary。
- 仍找不到就在 aura stderr 提示的位置加 `export AURA_RUNTIME_DIR=...` 显式覆盖。

## §X IR encoding baseline（Issue #375）

`IRInstruction`（`src/compiler/ir.ixx:141`）是 AoS layout：1 字节 opcode + 16 字节 fixed `array<uint32_t, 4>` operands + 9 个 metadata/字段。总计 **40 字节**（编译器验证：3 字节 padding 在 `linear_ownership_state`（1B）和 `adt_variant_id`（4B）之间）。大多数 instruction 用 1–3 个 operand,固定 4-slot 数组平均浪费 ~7.7 字节（按 2.08 operand/instr,sum-to baseline）。

**当前 baseline（sum-to 10, 2026-07-01）**：

| Field | Value |
|-------|-------|
| total-instructions | 46 |
| avg-operands-used | 2.08 |
| AoS bytes | 1840 |
| padding bytes | 138 (3 × 46) |
| unused-operand bytes | 232 |
| **compact projection (variable-length)** | **568 字节** |
| **compact ratio (vs AoS)** | **30.86%** → **69% reduction** ✅ |

**Compact 编码**（2 字节 header + 4 字节 per used operand,4 字节对齐）：opcode 8 位 + operand_count 4 位 + reserved 4 位。Metadata（type_id, shape_id, adt_variant_id, narrow_evidence, source_marker, linear_state）不进 hot-path encoding,作为 sidecar 留给 pass。

**观测接口**：
- C++：`aura::ir::compute_ir_stats(const IRModule&)` + `CompilerService::last_ir_stats()` （`service.ixx` snapshot,`last_ir_mod_` 赋值时同步算）。
- Aura：`(compile:ir-stats)` primitive 返回 hash。**限制**：primitive 自己 lower 会 clobber `last_ir_mod_`,所以从 `.aura` 调时看到的是 primitive 自己的 IR;从 C++ test API 调（`cs.last_ir_stats()`）看到的是上一个 workload。

**Pair with**：SoA skeleton `src/compiler/ir_soa.ixx`（#167 Phase 1,无 consumer）+ 测试 `tests/test_issue_375.cpp`（5 ACs / 23 tests）。

**§375 完整拆解**（scope-limited close ship Step A,defer Step B/C/D + 新 Step E）：
- **A** ✅ shipped：本节描述的 baseline + observability。
- **B** deferred：dual representation — `IRFunctionSoA` 加 `instructions_compact_` column（16-byte 紧凑 layout）+ `view_at_compact()` accessor。AoS 路径不变（pass 继续吃 AoS）,compact view 只读。需 1 session。
- **C** deferred：interpreter switch `ir_executor_impl.cpp:331` 改读 compact view。需先 B,1 session。
- **D** deferred：JIT lowering `aura_jit.cpp` / `aura_jit_bridge.cpp` 读 compact view 而非 AoS decode。1 session。
- **E** deferred：computed-goto dispatch,依赖 C 的 compact view。

**加新 baseline 任务时的检查清单**：
- [ ] 在 `tests/test_issue_375.cpp` 加一个 workload（fact / fib / hanoi-8 / map-fold 之一）+ 一个 AC（baseline 比 sum-to 大多少 / ratio 是否 ≥ AC）。
- [ ] 更新本节 baseline 表。
- [ ] 跑 `./build.py test issue` 全绿。

## §X Closure dispatch paths (Issue #252 + #376)

`Evaluator::apply_closure(ClosureId cid, args)`（`src/compiler/evaluator_eval_flat.cpp:65`）是 higher-order primitives（`map`/`filter`/`foldl`/`apply`）的中心分发。按 `cid` 类型分 4 条路径：

| Path | When | Cost | Counter |
|------|------|------|---------|
| **FFI** | `cid < ffi_runtime_.func_count()` | 直接调 native 函数指针,无 env 构造 | `closure_ffi_calls` |
| **Tree-walker** | `closures_` map hit（`cid → Closure`） | `materialize_call_env()` + `eval_flat()` | `closure_tw_calls` |
| **IR bridge** | `closure_bridge_` callback set（service.ixx L1930 设） | callback 透传 + 重建 Env | `closure_bridge_calls` |
| **IR direct** | IR interpreter 内部 `runtime_closures_` hit | 无 callback,直接 IR execute | `closure_ir_calls`（在 ir_executor_impl.cpp bump） |

**Epoch / stale 检查**：tree-walker path 入口调 `is_bridge_stale(bridge_epoch, current_bridge_epoch())`（`evaluator_eval_flat.cpp:194`），如果闭包的 bridge 过期则 `closure_stale_returns` 计数 +1，return `nullopt`。这是唯一一个 stale 检查点；IR direct path 不需要（runtime_closures_ 是 per-evaluator，所有者同生命周期）。

**当前 baseline**（`tests/test_issue_376.cpp` AC2/AC4 实测，2026-07-01）：

| Workload | calls-total | bridge-calls | bridge-pct | 路径分布 |
|----------|-------------|--------------|------------|---------|
| map 50-elem | 153 | 50 | **32%** | tw 主导,1/3 走 bridge |
| filter 50-elem | 153 | 50 | **32%** | tw 主导,1/3 走 bridge |
| foldl 50-elem | 103 | 0 | **0%** | 已在 fast path（binary closure 直接 inlined） |

**未来 fast path 目标**：把 map/filter 的 32% bridge-pct 降到 ~0%（bypass callback 走 IR direct path）。这需要：
1. closure 的 captured flat/pool 跟 IR 端 bridge 共享
2. bridge_epoch 检查保留为 fast path 入口的 lightweight check
3. AC：map/filter 的 bridge-pct 降 30+% + 1000-cycle mutation stress 无 stale-returns regression

**§376 完整拆解**（scope-limited close ship Slice A,defer Slice B）：
- **A** ✅ shipped：本节描述的 baseline + observability test。
- **B** deferred：fast path 重构（bypass `closure_bridge_` callback,对纯 IR 闭包直接调 `IRInterpreter::apply_closure`）。Hot-path 改动 + epoch 验证逻辑不能掉,需独立 session。先看本节 baseline 数据再 design。Pair with #375 Step C。

**观测接口**：
- C++：`cs.snapshot().closure_calls_total / closure_ffi_calls / closure_tw_calls / closure_ir_calls / closure_bridge_calls / closure_stale_returns`（来自 #252 ship 的 6 个 atomic counter）。
- Aura：`(closure:stats)` primitive 返回 hash，7 个字段：`calls-total / ffi-calls / tw-calls / ir-calls / bridge-calls / stale-returns / bridge-fraction-pct`。Aura primitive 自身 eval 路径会 bump 1-2 次 calls-total，所以与 C++ snapshot 之间有 drift（一般 ≤ 5），见 `test_issue_376.cpp` AC6。

**加新 closure 路径时**：
- [ ] 在 `apply_closure` 入口 bump `closure_calls_total`
- [ ] 在新路径 entry bump 对应的 path-specific counter
- [ ] 加 epoch/stale 检查（如果新路径会跨 bridge）
- [ ] 在 `tests/test_issue_376.cpp` 加 AC + 更新本节 baseline 表

## AI 扩展指南（Issue #711）

新 primitive 不必手写完整注册代码 — Aura 已经提供了 introspection + generator 的 Agent-friendly surface，让 Agent 从描述生成符合风格的 skeleton：

### 1. 查询 metadata

```scheme
;; #669 — enriched per-name meta (8 fields)
(hash-ref (query:primitives-meta "<primitive-name>")
  'name 'has-fn 'arity 'pure 'safety 'doc 'category 'schema)

;; #617 — registry-level catalog (7 fields)
(query:primitives-meta-catalog)
;; Includes by-category-eda / by-category-sva / by-category-verification
;; / by-category-general counts so the Agent can filter by domain.
```

### 2. 生成 skeleton

```scheme
;; #697 — AI-friendly primitive extension bundle
(primitive:generate-skeleton "coverpoint with bins")
;; → hash with 5 fields:
;;   category      "sva" | "verification" | "eda" | "general"
;;                 (heuristic dispatch from description keywords)
;;   spec          "(<args>) -> <ret>"  Aura signature snippet
;;   cpp-lambda    full add()/add_mutate() lambda with
;;                 MutationBoundaryGuard template
;;   test-snippet  example Aura invocation
;;   registration  DEFINE_PRIMITIVE_META(...) macro call
```

### 3. 注册风格

参考 [primitives-style.md](design/primitives-style.md)（Issue #671）：

- 优先 `add_mutate` over `add` for state changes（自动套 `MutationBoundaryGuard`）
- mutating 路径必须 `bump_*` 1 个 atomic counter（observability_metrics.h）
- 错误路径走 `PRIM_ERROR(...)` helper，不要直接 stderr
- `PrimMeta` 必须填：`arity / pure / safety_flags / doc / category`（`DEFINE_PRIMITIVE_META` macro）
- EDA/SVA/verification 方向 primitive 选 `kPrimSafetyMutates` + `category = "eda"|"sva"|"verification"`

### 4. 验证

注册之后：

```scheme
(query:primitives-extension-stats)  ;; #697 runtime counters
(query:primitives-meta-catalog)     ;; #617 should reflect new entry
(hash-ref (query:primitives-meta "<name>") 'has-fn)  ;; should be 1
```

详见 [`tests/test_issue_711.cpp`](../tests/test_issue_711.cpp) — closed-loop Agent sim 把 1-4 串起来 end-to-end。
