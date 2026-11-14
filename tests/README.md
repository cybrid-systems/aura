# tests/

How to run tests. How to add a test is [`HOMES.md`](HOMES.md) only.
Do not add new `test_issue_*` files. `python3 build.py gate` rejects a newly added one.

**Hot-path matrix:** [`STRATEGY.md`](STRATEGY.md) (#1887).
**Manifest policy:** [`COVERAGE.md`](COVERAGE.md).
**Generated inventory:** [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957).
**Fixture shard format:** [`fixtures/README.md`](fixtures/README.md).

---

## 哲学 (2026-07-24 重整)

**顶层目录镜像 `src/`** —— 每个 test 在哪个 `tests/<module>/` 下,直接对应 `src/<module>/`。
没有 "theme" / "domain" 抽象层,文件名前缀 `_unit_batch` 是聚合的明确信号,不是新的 dir 分类。

```
tests/
├── core/         ← src/core/*    (arena / ast / type / mutation / workspace_isolation / capability / sandbox / contracts / safety / audit / resource_quota / util)
├── parser/       ← src/parser/*
├── compiler/     ← src/compiler/* (jit / aot / ir / macro / observability / mutation_audit / evaluator / service / query / cache / value / type_checker / adt / ffi / messaging / lowering / pass / linear / shape / diag)
├── serve/        ← src/serve/*   (fiber / scheduler / gc / mailbox / orch_prim / async / http / util)
├── orch/         ← src/orch/*    (#1588)
├── reflect/      ← src/reflect/*
├── (renderer/ removed — #2625/#2626)
├── repl/         ← src/repl/*
├── stdlib/       ← src/stdlib/*
├── (tui/ removed — #2626)
└── (横向支撑)
    ├── e2e/      跨模块 E2E
    ├── bench/    SLO bench (C++ + .aura)
    ├── fuzz/     fuzz orchestrator + corpus
    ├── fixtures/ 共享 case 数据
    ├── python/   harness + gate + runners
    ├── regression/ .aura 回归 fixture
    └── scaffolds/ 起步模板 (不编译, R13+R14 改名)
```

## `.aura` file leaves

`suite` and `regression` share `tests/python/aura_file_runner.py`:

| Leaf | Directory | How it is invoked | Pass rule |
|---|---|---|---|
| `suite` | `tests/suite/*.aura` | `aura --load file` | exit 0, unless `;; expect-status:` / `;; expect-err:` (same rules as SnippetSpec); optional first `;; expect:` |
| `regression` | `tests/regression/*.aura` | stdin (leading `;;` header stripped) | first `;; expect:` (`no-crash` / `no-error` / `no-timeout` / substring) |
| `integ` | `tests/fixtures/integ/*.json` | stdin + `--ir`/`--typecheck`/`--serve` | status + substring / `>=N` / err needle |
| `p0` snippets | `tests/fixtures/regression/*.json` | stdin (serial; shares `/tmp` seeds) | substring or exact `(…)` out; regex err |
| `typecheck` | benchmark fixtures with `typecheck_suite` | stdin + `--typecheck` | a `type:` line contains expected type |
| `smoke` | `tests/fixtures/smoke.json` | `bash -c` command | substring in stdout+stderr |
| `e2e` | `tests/e2e/commercial_readiness/` | `aura --load` via `invoke_aura_load` | E2E-PASS + golden labels |

`p0` still runs freeze/AOT/JIT/module extras in `tests/python/test_regression.py` after the snippets.

Aura L1 cases live in `lib/std/test.aura` (`test-case` / `test-finish`).
`;; expect-status:` and `;; expect-err:` on a suite file are judged like
`SnippetSpec`. `std/edsl-test-harness` is a compatibility shim.
`python3 tests/run.py home "<query>"` prints the theme file and an append
snippet. `cover --issue N` and `cover --name <symbol>` list cases that
already cover it. `affected` is the local fast loop: the workspace diff
(or explicit paths) selects that L1 suite and Python theme, plus
`std/test` and smoke. It does not compile the issue-farm. An unmapped
diff exits non-zero. See [`HOMES.md`](HOMES.md).

## 命名约定

| 类别 | 例 | 说明 |
|---|---|---|
| 单元 / 主题套件 | `test_linear_cross_closure.cpp` | `<module>_<feature>` — **无 issue 后缀** |
| 批次聚合 | `test_jit_aot_hot_update_unit_batch.cpp` | 多 issue AC 合并 |
| Bench | `bench_jit_orc_compile.cpp` | SLO gate 用 |
| E2E / 回归 `.aura` | `jit_deopt_basic.aura` | 不带数字前缀 |

**Issue 号:** 只写在文件头 `@reason` / `// Issue #NNNN` 和
`scripts/coverage/manifests/<N>.json`。历史遗留的 `test_*_NNNN.cpp` 按
[`HOMES.md`](HOMES.md) 分波并入主题套件；**禁止新增**。

## 添加新测试

见 [`HOMES.md`](HOMES.md)。先跑 `python3 tests/run.py home "<query>"`。

## Harness

```cpp
#include "test_harness.hpp"   // #1960 统一 harness,tests/ 在 include path
```

`CHECK` / `EXPECT_*` · `TEST` / `RUN_ALL_TESTS` · `run_pilot_tests()` ·
`aura_call_expr()` · `k_int_env()` · `AURA_ISSUE_TEST` (bundle entry) ·
`capture_stable_refs` / `validate_stable_refs` (FlatAST helpers).

`issue_test_harness.hpp` 是 **deprecated shim** —— 不要在新代码里用。

## CMake 解析顺序

`aura_resolve_test_cpp(NAME)` (cmake/AuraTest.cmake) 按以下顺序搜源文件:
1. `tests/<NAME>.cpp` (legacy fallback)
2. `tests/*/${NAME}.cpp` (一级深度,覆盖 tests/compiler/test_X.cpp 等)
3. `tests/domain/${NAME}.cpp` (legacy)
4. `tests/domain/*/${NAME}.cpp` (legacy)

注册宏:
```cmake
aura_add_issue_test(test_<feature>)                        # 默认 C++20 模块
aura_issue_test_link_light(test_<feature>)                 # 默认: 无 LLVM 的 ABI/stub (~MB)
aura_issue_test_link_llvm_jit(test_<feature>)              # 仅当需要真实 OrcJIT / emit_native
aura_add_issue_test_reflect_standalone(test_<feature>)     # 仅反射 (无完整链接)
aura_add_issue_test_standalone(test_<feature>)             # 无 C++ modules
```


### Link profiles (磁盘 / 链接成本)

| Helper | 体积 (约) | 何时用 |
|--------|-----------|--------|
| `aura_issue_test_link_light` | ~0.2–15 MB | **默认**。CompilerService / FlatAST / source-cite / 非 OrcJIT |
| `aura_issue_test_link_llvm_jit` | ~70 MB | 真实 JIT 编译、AOT emit、SpecJIT、native deopt 表 |

- 全量 LLVM 链 ~350×70MB ≈ **19GB** `build/test_*`；light 后中位可到 **~MB 级**。
- **P2 shared libs**: `aura_test_objects` / `aura_jit_light_test_objects` /
  `aura_jit_test_objects` / `aura-reflect` are **SHARED** (`.so` in
  `build/`). Binaries keep `BUILD_RPATH=$BUILD_DIR` so `ninja`/`ctest`
  resolve them without `LD_LIBRARY_PATH`. Full-JIT `.so` embeds LLVM once.
- **P2 shared libs**: `aura_test_objects` / `aura_jit_light_test_objects` /
  `aura_jit_test_objects` / `aura-reflect` are **SHARED** (`.so` in
  `build/`). Binaries keep `BUILD_RPATH=$BUILD_DIR` so `ninja`/`ctest`
  resolve them without `LD_LIBRARY_PATH`. Full-JIT `.so` embeds LLVM once.
- full-LLVM allow-list: `cmake/issue_tests_need_full_llvm.txt`（新 JIT 测试加入此表）。
- **禁止**新 standalone 默认挂 `link_llvm_jit`。新 AC 优先:
  1. 扩已有 `test_*_unit_batch` / domain batch
  2. 否则 `link_light` standalone
  3. 仅当断言真实 native/JIT 时才 `link_llvm_jit`

## 运行

```bash
python3 tests/run.py list
python3 tests/run.py affected          # diff → L1 home + std/test + smoke
python3 tests/run.py issues --tier fast
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
python3 tests/run.py bench
python3 tests/run.py mutation

./build.py check              # gate + build + ci
./build.py gate               # static only
./build.py list               # tiers + fast loop + leaf suites (SSOT)
./build.py test               # default: ci (core + safety + issues)
./build.py test affected      # same fast loop as tests/run.py affected
./build.py test pr            # core + safety + issues-fast
./build.py test unit | integ | issues | issues-fast
./build.py test suite | regression   # shared .aura file runner


ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix
ninja -C build test_arena_batch test_gc_compact_batch  # EXCLUDE_FROM_ALL 目标
```

## 不要做

- 不要再开 `tests/domain/`、`tests/arena/`、`tests/edsl/`、`tests/compiler_core/`、`tests/jit/`、`tests/fiber/`、`tests/mutation/`、`tests/observability/`、`tests/linear/`、`tests/shape/`、`tests/misc/`、`tests/stdlib/` 等已经废弃的 theme 目录。Aura 行为用例追加到已有的 `tests/suite/`（见 HOMES.md）
- 不要再写带 issue 号的新文件（`test_issue_*`、`*_<数字>.py`、`tests/suite/*_<数字>.aura`、`check_*_<数字>.py`）。`python3 build.py gate` 会拒绝新增；改已有文件可以。
- 不要再写 `docs/design/NNNN-*.md` (per Anqi #1655 哲学:agent 仓库 plan 走 chat)
- 顶层 `tests/<NAME>.cpp` 只在 fallback 时用 (老测试未迁移完),新代码必须放 `tests/<module>/` 下
- 新 AC **先搜再写**：`rg` 主题关键词 → 扩现有 suite，不要新开 issue 号文件

## Related

| Doc | Purpose |
|-----|---------|
| [`HOMES.md`](HOMES.md) | **How to add a test** — the one decision tree and the genealogy |
| [`CONSOLIDATION_PLAN.md`](CONSOLIDATION_PLAN.md) | Historical snapshot of C++ issue-suffix renames |
| [`STRATEGY.md`](STRATEGY.md) | Hot-path matrix only (#1887), not the how-to |
| [`legacy_test_inventory.md`](legacy_test_inventory.md) | #1957 generated inventory, not the how-to |
| [`root_test_classification.md`](root_test_classification.md) | Theme → module map + near-dups |
| [`fixtures/README.md`](fixtures/README.md) | Sharded fixture format + 12 KB / 50-case rule |
| [`core/arena_pilot_notes.md`](core/arena_pilot_notes.md) | #1959 arena pilot 笔记 (历史参考) |
| [`scaffolds/module_test_scaffold.cpp`](scaffolds/module_test_scaffold.cpp) | C++ copy-paste scaffold. The decision tree is HOMES.md |
| [`scaffolds/legacy_test_redirect.cpp`](scaffolds/legacy_test_redirect.cpp) | Legacy redirect (STOP banner + 5-step tree) |
| [`../docs/test_harness_pattern.md`](../docs/test_harness_pattern.md) | CMake resolve order + harness policy |
| [`../docs/contributing.md`](../docs/contributing.md) | Repo entry → testing + workflow |