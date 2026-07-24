# tests/

How and where to add tests in Aura.

**Strategy & hot-path coverage:** [`STRATEGY.md`](STRATEGY.md) (#1887).
**Live layout detail:** [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957).
**Fixture shard format:** [`fixtures/README.md`](fixtures/README.md).

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
├── renderer/     ← src/renderer/* (engine / voxel / tui)
├── repl/         ← src/repl/*
├── stdlib/       ← src/stdlib/*
├── tui/          ← src/tui/*
└── (横向支撑)
    ├── e2e/      跨模块 E2E
    ├── bench/    SLO bench (C++ + .aura)
    ├── fuzz/     fuzz orchestrator + corpus
    ├── fixtures/ 共享 case 数据
    ├── python/   harness + gate + runners
    ├── regression/ .aura 回归 fixture
    └── _templates/ 起步模板 (不编译)
```

## 命名约定

| 类别 | 例 | 说明 |
|---|---|---|
| 单元测试 | `test_jit_orc_deopt.cpp` | `<module>_<feature>` 二级分类 |
| 批次聚合 | `test_jit_aot_hot_update_unit_batch.cpp` | 多 AC 合并 |
| Bench | `bench_jit_orc_compile.cpp` | SLO gate 用 |
| E2E `.aura` | `jit_deopt_basic.aura` | 按 module 命名 |
| 回归 `.aura` | `closure_let_dangling.aura` | 不带数字前缀 |

**issue 号处理**:主放 banner header (`// Issue #NNNN (#1978 renamed): issue# moved from filename to header.`)。
文件名只在消歧义价值时带 issue 后缀。

## 添加新测试 (最短路径)

```
新 query:*-stats / engine:* schema gate?
  └─ YES → tests/compiler/obs_schema_cases.hpp 加一行 + tests/compiler/production_sweep_cases.hpp (production flag)
           跑 ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix. STOP.

新 stats surface (编译器/类型/shape)?
  └─ tests/compiler/test_<feature>_<issue>.cpp (单 AC)
     或扩 tests/compiler/test_<theme>_unit_batch.cpp (多 AC 聚合).

新 behavioral gate (fiber / hygiene / typed mutate)?
  └─ tests/serve/test_fiber_<feature>.cpp 或 tests/compiler/test_<feature>_unit_batch.cpp.

跨模块 hot-path 场景?
  └─ tests/core/test_hotpath_matrix_batch.cpp (Single mega binary).

新 E2E .aura?
  └─ tests/e2e/<feature>.aura 或 tests/suite/<feature>.aura

新 benchmark?
  └─ tests/bench/<module>/bench_<feature>.cpp 或 tests/bench/bench_<lang_bench>.aura
```

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
aura_issue_test_link_llvm_jit(test_<feature>)              # 加 LLVM JIT 链接
aura_add_issue_test_reflect_standalone(test_<feature>)     # 仅反射 (无完整链接)
aura_add_issue_test_standalone(test_<feature>)             # 无 C++ modules
```

## 运行

```bash
python3 tests/run.py list
python3 tests/run.py issues --tier fast
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
python3 tests/run.py bench
python3 tests/run.py mutation

./build.py check              # gate + build + default tests
./build.py gate               # static only
./build.py test unit | integ | issues | issues-fast

ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix
ninja -C build test_arena_batch test_gc_compact_batch  # EXCLUDE_FROM_ALL 目标
```

## 不要做

- 不要再开 `tests/domain/`、`tests/arena/`、`tests/edsl/`、`tests/compiler_core/`、`tests/jit/`、`tests/fiber/`、`tests/mutation/`、`tests/observability/`、`tests/linear/`、`tests/shape/`、`tests/misc/`、`tests/templates/`、`tests/stdlib/`、`tests/suite/` 等 theme-named dir
- 不要再写 `tests/test_issue_NNNN.cpp` (legacy 模式,新代码写 `tests/<module>/test_<feature>_<issue>.cpp`)
- 不要再写 `docs/design/NNNN-*.md` (per Anqi #1655 哲学:agent 仓库 plan 走 chat)
- 顶层 `tests/<NAME>.cpp` 只在 fallback 时用 (老测试未迁移完),新代码必须放 `tests/<module>/` 下

## Related

| Doc | Purpose |
|-----|---------|
| [`STRATEGY.md`](STRATEGY.md) | Hot-path coverage matrix + SLO goals (#1887) |
| [`legacy_test_inventory.md`](legacy_test_inventory.md) | #1957 inventory + migration waves |
| [`root_test_classification.md`](root_test_classification.md) | Theme → module map + near-dups |
| [`fixtures/README.md`](fixtures/README.md) | Sharded fixture format + 12 KB / 50-case rule |
| [`core/arena_pilot_notes.md`](core/arena_pilot_notes.md) | #1959 arena pilot 笔记 (历史参考) |
| [`../docs/test_harness_pattern.md`](../docs/test_harness_pattern.md) | CMake resolve order + harness policy |
| [`../docs/contributing.md`](../docs/contributing.md) | Repo entry → testing + workflow |
