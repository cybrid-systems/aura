# Issue #1640 — AOT bridge mangle versioning + region filtering + incremental re-emit + hot-update observability

## 来源
Runtime 多 fiber 并发 + Mutation 安全 生产级 Code Review (2026-07-16)
+ JIT/AOT 路径成熟度分析 (2026-07-19). Building on the dual-epoch
fence + `aot_emit_version` + `aura_aot_fn_version_is_stale` + JIT
Apply prologue `is_fn_epoch_stale` / `is_env_frame_stale` foundation
laid by #136 / #243 / #708 / #1262 / #1262 / #1367 / #1369 / #1480.

## 问题描述
`aura_jit_bridge.cpp` 的 `mangle_aot_name` (带 emit_version)、
`generate_registration_c` (发射 aot_emit_version + aot_fn_versions[] +
aot_region_mask)、`aura_reload_aot_module_for_eval` (version/region/
stale 检测 + atomic rollback) 已提供良好基础，支持 dual-epoch
(bridge_epoch + defuse_version) 检查和 hot-reload。但：

- `mangle_aot_name` 未充分 stamping EnvFrame version / linear state，
  导致 closure bridge hot-update 后可能 stale（捕获 env 漂移但
  symbol 还匹配旧 env）。
- `aura_reload_aot_module_for_eval` 缺 env_frame_version drift
  检测，captured-env 漂移无法触发 graceful fallback。
- incremental re-emit 钩子缺失，hot-swap 失败时 observability 不足
  （诊断难：no graceful deopt + re-emit 策略 + 细粒度 metrics）。
- 多 agent orchestration 下 region mask 隔离 + version 漂移
  风险（已有 region check，缺 env_frame_version 配套）。

## 代码证据 (code anchors)

### 修复前

```cpp
// aot_mangle.h — pre-#1640
inline std::string mangle_aot_name(const std::string& original,
                                   std::uint32_t disambiguator,
                                   std::uint64_t defuse_version = 0) {
    // ... only stamps disambiguator + defuse_version ...
}

// aura_jit_bridge.cpp aura_reload_aot_module_for_eval — pre-#1640
//   1. dlopen + dlerror log
//   2. aot_emit_version stale reject (binary < host)
//   3. no aot_emit_version case (pre-#243 binary)
//   4. region isolation check (binary_region != host_region)
//   5. defuse_version stale check (binary < host_defuse)
//   NO env_frame_version drift check
//   NO incremental_reemit hook
//   NO aot_env_frame_version_drift_prevented /
//      aot_incremental_reemit_triggered metrics
```

### 修复后 (Issue #1640 hardened)

```cpp
// aot_mangle.h — post-#1640
inline std::string mangle_aot_name(const std::string& original,
                                   std::uint32_t disambiguator,
                                   std::uint64_t defuse_version = 0,
                                   std::uint64_t env_frame_version = 0,
                                   std::uint8_t linear_state = 0) {
    // ... existing logic ...
    // New suffix: `_e<env_frame_version>_l<linear_state>`
    //   (only appended when env_frame_version != 0 || linear_state != 0
    //    for backwards compat with pre-#1640 binaries)
}

// aura_jit_bridge.cpp aura_reload_aot_module_for_eval — post-#1640
//   ... existing checks 1-5 unchanged ...
//   6. NEW env_frame_version drift check:
//      - dlsym aot_env_frame_version
//      - if binary_env_ver < host_env_ver:
//        - bump aot_env_frame_version_drift_prevented
//        - bump aot_incremental_reemit_triggered
//        - rollback_close + return false (graceful fallback to JIT)
//   7. success path bumps aot_hot_update_success_ +
//      aot_hot_update_multi_agent_versioned (unchanged)

// aura_jit_bridge.cpp generate_registration_c — post-#1640
//   Threads env_frame_version + linear_state through mangle_aot_name
//   call for each function. Emits aot_env_frame_version symbol
//   so the runtime can dlsym it during reload.
```

## 精确改动位置 (file-by-file)

1. **src/compiler/aot_mangle.h** — extend `mangle_aot_name` signature
   with `env_frame_version` + `linear_state` params (defaults preserve
   backwards compat). Append `_e<N>_l<N>` suffix when either is non-zero.

2. **src/compiler/aura_jit_bridge.cpp::aura_reload_aot_module_for_eval**
   — add env_frame_version drift check (after defuse_version stale
   check at line ~1343). On drift: bump 2 new metrics, rollback_close,
   return false (graceful fallback to JIT).

3. **src/compiler/aura_jit_bridge.cpp::generate_registration_c** —
   thread env_frame_version + linear_state through mangle_aot_name
   call for each function. Emit `aot_env_frame_version` symbol so
   the runtime can dlsym it during reload.

4. **src/compiler/observability_metrics.h** — add 2 atomic counter
   slots (`aot_env_frame_version_drift_prevented` +
   `aot_incremental_reemit_triggered`).

5. **src/compiler/compiler_metrics_fields.inc** — add 2 X-macro
   fields matching the new counters.

6. **src/compiler/evaluator.ixx** — declare 2 bump_/getter pairs
   for the new metrics.

7. **tests/test_aot_hot_update_incremental.cpp** (new, ~280 lines,
   7 ACs source-driven). Pattern: tests/test_incremental_relower_perblock.cpp
   + tests/test_aot_hotupdate_versioning.cpp.

8. **scripts/check_aot_hot_update_incremental_coverage.py** (new,
   ~150 lines, 10 ACs linter).

9. **CMakeLists.txt** — add `aura_add_issue_test(test_aot_hot_update_incremental)`
   + `aura_issue_test_link_llvm_jit(test_aot_hot_update_incremental)`
   entries.

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_aot_hot_update_incremental` AC1: `mangle_aot_name` signature extended with `env_frame_version` + `linear_state` params + `_e<N>_l<N>` suffix |
| AC2 | AC2: 2 metric slots in `observability_metrics.h` |
| AC3 | AC3: 2 X-macro fields in `compiler_metrics_fields.inc` |
| AC4 | AC4: 2 `bump_/get_` pairs declared in `evaluator.ixx` |
| AC5 | AC5: `aura_reload_aot_module_for_eval` wires env_frame_version drift check + 2 metric bumps + rollback |
| AC6 | AC6: `generate_registration_c` emits `aot_env_frame_version` + threads env_frame_version + linear_state through mangle |
| AC7 | AC7: cross-layer baseline regression — `CompilerService` round-trip still works |

10 linter ACs in `scripts/check_aot_hot_update_incremental_coverage.py`:
production-code wire-up checks for the 6 wire-up sites + metric slot
presence + bump_/getter pair presence + X-macro fields + mangle signature
+ drift detection + 2 metric bumps + registration emit.

## 预期收益
- AOT 热更新可观测、可回滚、支持 incremental re-emit,生产部署-ready。
- 多 agent 下更强 isolation + env_frame_version stamping。
- 构建在当前 dual-epoch 之上,快速落地(`mangle_aot_name` extension +
  reload drift check + 2 new metrics,无需重新设计 AOT bridge)。
- 捕获 env 漂移可被 runtime 在 reload 时检测,避免 stale closure
  bridge 激活(paired with `is_env_frame_stale` / `is_fn_epoch_stale`
  in JIT Apply prologue per #1631)。

## 优先级
**P1**（AOT 成熟度提升,部署可行性关键）

## 标签
P1, deployment, AOT, JIT, hot-update, versioning,
production-readiness, dual-epoch

## 相关 Issues
- #136 (mangle_aot_name original)
- #243 (aot_emit_version symbol)
- #708 (region mask multi-agent isolation)
- #1262 (defuse_version stale check on reload)
- #1367 (per-agent AotState for multi-agent isolation)
- #1369 (always append `_vN`)
- #1480 (incremental re-emit scaffolding)
- #1601, #1605, #1623 (incremental re-lower observability lineage —
  paired pattern for the AOT side)
- #1631 (JIT dual-epoch fence with is_env_frame_stale / is_fn_epoch_stale)

## 验证方式
- `tests/test_aot_hot_update_incremental.cpp`: 7 ACs all green
- `scripts/check_aot_hot_update_incremental_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing AOT metrics surface extended within 521 budget per #1734
  raise) + test-registry (#1572) + test binding + coverage (#1453)
- Same PR cycle as #1908 / #1907 / #1637 / #1638 / #1639: edit → build →
  run tests → descriptive commit → push `main` (direct, no PR review
  per MEMORY.md workflow).