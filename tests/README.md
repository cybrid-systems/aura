# tests/

How and where to add tests in Aura.

**Strategy & hot-path coverage:** [`STRATEGY.md`](STRATEGY.md) (#1887).
**Live layout detail:** [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957).
**Fixture shard format:** [`fixtures/README.md`](fixtures/README.md).

---

## ⛔ STOP — Before you write a test, follow this decision tree

**Canonical home map + agent rules:** [`HOMES.md`](HOMES.md) (read first).

```
═══════════════════════════════════════════════════════════════════════════
 STOP — extend an existing thematic suite; do NOT invent test_*_<issue>.cpp
═══════════════════════════════════════════════════════════════════════════

1. New query:*-stats / engine:* schema gate?
   └─ YES → tests/compiler/obs_schema_cases.hpp 加一行
            (tests/compiler/production_sweep_cases.hpp if production flag)
            跑 ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix. STOP.

2. Same feature family already has a suite? (see HOMES.md)
   └─ YES → open that file, add AC + wire into main/run_all. STOP.
   └─ Examples: test_linear_cross_closure, test_arena_batch,
                test_mutation_occurrence_dirty_batch, test_obs_schema_matrix

3. Fits a batch / matrix suite?
   └─ YES → extend tests/<module>/test_<theme>_batch.cpp (or unit_batch). STOP.

4. Truly new feature surface (no home)?
   └─ copy scaffolds/module_test_scaffold.cpp →
      tests/<src-module>/test_<module>_<feature>.cpp
      (module + feature ONLY — NO trailing issue number)
   └─ CMake: aura_add_issue_test + link_light + all_test_issue_targets
   └─ Issue #N goes in the file banner comment only.

5. HARD BANS (pre-commit rejects new files matching these):
   ✗ tests/issues/test_issue_N.cpp
   ✗ tests/**/test_issue_*.cpp
   ✗ tests/**/test_*_<3–5 digit>.cpp   e.g. test_foo_2622.cpp
   ✗ new tests/test_*.cpp at repo root
```

**Templates:** [`scaffolds/module_test_scaffold.cpp`](scaffolds/module_test_scaffold.cpp) · [`HOMES.md`](HOMES.md)

**Why?** `tests/<module>/` mirrors `src/<module>/`. Issue numbers in filenames
spawn one binary per ticket and block consolidation. Coverage stays in
`scripts/coverage/manifests/<N>.json` + banner comments.

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
├── tui/          ← src/tui/*
└── (横向支撑)
    ├── e2e/      跨模块 E2E
    ├── bench/    SLO bench (C++ + .aura)
    ├── fuzz/     fuzz orchestrator + corpus
    ├── fixtures/ 共享 case 数据
    ├── python/   harness + gate + runners
    ├── regression/ .aura 回归 fixture
    └── scaffolds/ 起步模板 (不编译, R13+R14 改名)
```

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

## 添加新测试 (展开)

见 [`HOMES.md`](HOMES.md)。短版：

1. schema → `obs_schema_cases.hpp`
2. 有主题家 → **改那个文件**
3. 没有家 → `test_<module>_<feature>.cpp`（无 issue 号）+ CMake light link

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

- 不要再开 `tests/domain/`、`tests/arena/`、`tests/edsl/`、`tests/compiler_core/`、`tests/jit/`、`tests/fiber/`、`tests/mutation/`、`tests/observability/`、`tests/linear/`、`tests/shape/`、`tests/misc/`、`tests/stdlib/`、`tests/suite/` 等 theme-named dir
- 不要再写 `tests/test_issue_NNNN.cpp` / `tests/**/test_*_NNNN.cpp`（pre-commit hard fail）
- 不要再写 `docs/design/NNNN-*.md` (per Anqi #1655 哲学:agent 仓库 plan 走 chat)
- 顶层 `tests/<NAME>.cpp` 只在 fallback 时用 (老测试未迁移完),新代码必须放 `tests/<module>/` 下
- 新 AC **先搜再写**：`rg` 主题关键词 → 扩现有 suite，不要新开 issue 号文件

## Related

| Doc | Purpose |
|-----|---------|
| [`HOMES.md`](HOMES.md) | **Agent 必读** — theme → suite map + hard bans + consolidation waves |
| [`STRATEGY.md`](STRATEGY.md) | Hot-path coverage matrix + SLO goals (#1887) |
| [`legacy_test_inventory.md`](legacy_test_inventory.md) | #1957 inventory + migration waves |
| [`root_test_classification.md`](root_test_classification.md) | Theme → module map + near-dups |
| [`fixtures/README.md`](fixtures/README.md) | Sharded fixture format + 12 KB / 50-case rule |
| [`core/arena_pilot_notes.md`](core/arena_pilot_notes.md) | #1959 arena pilot 笔记 (历史参考) |
| [`scaffolds/module_test_scaffold.cpp`](scaffolds/module_test_scaffold.cpp) | R17 rewrite, 5-step decision tree + copy-paste checklist |
| [`scaffolds/legacy_test_redirect.cpp`](scaffolds/legacy_test_redirect.cpp) | Legacy redirect (STOP banner + 5-step tree) |
| [`../docs/test_harness_pattern.md`](../docs/test_harness_pattern.md) | CMake resolve order + harness policy |
| [`../docs/contributing.md`](../docs/contributing.md) | Repo entry → testing + workflow |