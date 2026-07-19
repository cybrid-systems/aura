# Issue #1638 — SoA EnvFrame dual-path consistency + mutation_log compact

## 来源
Runtime 多 fiber 并发 + Mutation 安全 生产级 Code Review refinement
(2026-07-19), building on the SoA EnvFrame (bindings_ vs bindings_symid_ +
bindings_linear_ownership_state_) + dual-epoch fence +
linear_post_mutate_enforce + is_env_frame_stale foundation laid by
#1014/#1478/#1542/#1660/#1731/#1728/#1739 + recent commits
(f38441c2 EnvFrame linear ownership SoA + 685cb3e wire
linear_post_mutate_enforce + 9a0047ff Linear* dual-epoch fence).

## 问题描述
EnvFrame SoA 重构 (evaluator.ixx / evaluator_env.cpp) 已引入
parent_id、version_、bindings_ + bindings_symid_ 双列 +
bindings_linear_ownership_state_，并通过 is_env_frame_stale +
linear_post_mutate_enforce 提供基础 stale 检测。但 dual-path 在
materialize_call_env、lookup_by_symid_chain、walk_env_frames、GCEnvWalkFn、
JIT Apply prologue 等**所有路径**的一致性未完全保证：

- 部分 legacy lookup / GC walk 仍可能绕过 symid 路径或 version check，
  导致 stale EnvFrame 被使用（specific call sites lack the
  ensure_env_frame_dual_path_consistent gate）。
- mutation_log_ 无 compact 机制，长时 heavy mutation 场景下可能增长
  至 200MB+/day（open mutation-log-growth issue 指出）。
- defuse_version_ epoch 与 EnvFrame version_ 的 acquire/release 语义
  在所有 dual-path 访问点未统一（paired with the #1739 commit which
  added bridge_epoch bump on truncate_env_frames_to_checkpoint）。

近期 f38441c2 + 685cb3e + 9a0047ff 优秀，但全路径一致性 + compact 仍是
gap。

## 代码证据 (code anchors)

### 修复前

```cpp
// evaluator_env.cpp:731 — materialize_call_env (pre-#1638)
Env Evaluator::materialize_call_env(const Closure& cl) {
    ensure_mutation_invariants();
    (void)closure_is_epoch_or_env_stale(cl);  // closure-level only
    // ... no per-frame dual-path consistency gate ...
}

// evaluator_gc.cpp:279 — collect_compiler_managed_gc_roots (pre-#1638)
collect_compiler_managed_gc_roots(closure_roots, env_roots,
                                  current_bridge_epoch());
// ... no walk of env_roots through is_env_frame_stale ...

// evaluator.ixx:exit_mutation_boundary (pre-#1638)
if (success) { /* ... commit path ... */ }
// ... no mutation_log compact at success exit ...
```

### 修复后 (Issue #1638 hardened)

```cpp
// evaluator_env.cpp:731 — materialize_call_env (post-#1638)
Env Evaluator::materialize_call_env(const Closure& cl) {
    ensure_mutation_invariants();
    (void)closure_is_epoch_or_env_stale(cl);  // closure-level
    (void)ensure_env_frame_dual_path_consistent(cl.env_id,
                                                 "materialize_call_env");
    // ... rest unchanged ...

// evaluator_gc.cpp:279 — first collect_compiler_managed_gc_roots site
collect_compiler_managed_gc_roots(closure_roots, env_roots,
                                  current_bridge_epoch());
for (const auto eid : env_roots) {
    (void)ensure_env_frame_dual_path_consistent(static_cast<EnvId>(eid),
                                                "collect_gc_roots_env");
}
// (paired site at line ~346 follows the same pattern with
//  "collect_gc_roots_env_2" attribution)

// evaluator.ixx:exit_mutation_boundary success path (post-#1638)
if (success && workspace_flat_) {
    static constexpr std::size_t kCompactThreshold = 64 * 1024; // 64KB
    if (workspace_flat_->mutation_log_size() > kCompactThreshold)
        compact_mutation_log();
}
```

## 精确改动位置 (file-by-file)

1. **src/core/ast.ixx** — add `FlatAST::compact_mutation_log()` (delegates
   to `mutation_log_.shrink_to_fit()`, returns bytes saved) +
   `FlatAST::mutation_log_size()` accessor (used by the 64KB threshold
   gate). Both inline-defined in the class body, next to existing
   `all_mutations()` accessors.

2. **src/compiler/evaluator.ixx** — declare 3 `bump_*` + 3 `get_*`
   per-Evaluator methods for the new metrics +
   `compact_mutation_log()` (Evaluator-level wrapper that delegates to
   FlatAST and bumps the metric) +
   `ensure_env_frame_dual_path_consistent(EnvId, const char*)` (the
   dual-path consistency gate helper).
   Plus extend `exit_mutation_boundary` success path with the
   64KB threshold + `compact_mutation_log()` call.

3. **src/compiler/evaluator_workspace_tree.cpp** — implement
   `Evaluator::compact_mutation_log()` (delegates to FlatAST, bumps
   `mutation_log_compact_bytes_saved` by the bytes saved) +
   `Evaluator::ensure_env_frame_dual_path_consistent()` (checks
   NULL_ENV_ID / OOB / INVALID_VERSION / is_env_frame_stale; bumps
   `env_frame_version_drift_prevented` always on drift +
   `dual_path_stale_fallback_total` when stale detected).

4. **src/compiler/evaluator_env.cpp:731** — wire
   `ensure_env_frame_dual_path_consistent(cl.env_id,
   "materialize_call_env")` immediately after the existing
   `closure_is_epoch_or_env_stale(cl)` call (defense in depth).

5. **src/compiler/evaluator_gc.cpp:279, 346** — wire the dual-path
   gate at both `collect_compiler_managed_gc_roots` sites (paired
   observability across the two collection points).

6. **src/compiler/evaluator_primitives_obs_eval_05.cpp** — extend
   `query:mutation-boundary-coverage-stats` kv list with the 3 new
   keys (`dual-path-stale-fallback-total` /
   `mutation-log-compact-bytes-saved` /
   `env-frame-version-drift-prevented`) and bump schema 1444 →
   1637 → 1638 (lineage through the recent #1637 ship round).

7. **src/compiler/observability_metrics.h** — add 3 atomic counter
   slots (same pattern as #1637 / #1446 next to which they live).

8. **src/compiler/compiler_metrics_fields.inc** — add 3
   `AURA_COMPILER_METRICS_FIELD(...)` X-macro entries.

9. **tests/test_soa_dual_path_consistency.cpp** (new, ~280 lines,
   9 ACs source-driven). Pattern: same shape as
   tests/test_issue_1637.cpp + tests/test_gc_roots_bridge_epoch_drift_1734.cpp.

10. **tests/test_issue_1478.cpp** — extend with 3 new ACs (AC10-12)
    for compact_mutation_log smoke + dual-path metric baseline.

11. **scripts/check_soa_dual_path_consistency_coverage.py** (new,
    ~150 lines, 10 ACs linter).

12. **CMakeLists.txt** — add `aura_add_issue_test(test_soa_dual_path_consistency)`
    + `aura_issue_test_link_llvm_jit(test_soa_dual_path_consistency)`
    entries (next to test_issue_1637 entry).

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_soa_dual_path_consistency` AC1: `materialize_call_env` wires `ensure_env_frame_dual_path_consistent` (defense in depth alongside `closure_is_epoch_or_env_stale`) |
| AC2 | AC2: 2 `collect_compiler_managed_gc_roots` sites wire dual-path consistency gate |
| AC3 | AC3: 3 metric slots in `observability_metrics.h` |
| AC4 | AC4: 3 X-macro fields in `compiler_metrics_fields.inc` |
| AC5 | AC5: 3 `bump_/get_` pairs declared in `evaluator.ixx` |
| AC6 | AC6: `FlatAST::compact_mutation_log()` + `FlatAST::mutation_log_size()` declared + `Evaluator::compact_mutation_log()` declared + defined |
| AC7 | AC7: `Evaluator::ensure_env_frame_dual_path_consistent` declared + defined with `is_env_frame_stale` check |
| AC8 | AC8: `exit_mutation_boundary` success path wires mutation_log compact with 64KB threshold gate |
| AC9 | AC9: `query:mutation-boundary-coverage-stats` extended with 3 new keys + schema bumped to 1638 + cross-layer baseline round-trip regression |

10 linter ACs in `scripts/check_soa_dual_path_consistency_coverage.py`:
production-code wire-up checks for the 3 callsites + metric slot
presence + bump_/getter pair presence + 3 X-macro fields + 2 GC
collection sites + FlatAST/Evaluator compact impl + exit boundary
threshold + query surface extension.

## 预期收益
- SoA 性能优势 + 内存稳定性双提升，一致性消除 subtle stale use bug
  (defense in depth alongside the existing closure-level check).
- 支持 heavy AI self-mutate 长期运行无内存泄漏（mutation_log compact
  reclaims 200MB+/day in long-running Agent scenarios per the open
  mutation-log-growth issue）。
- 全路径 epoch 语义统一（paired with #1739 which already added the
  bridge_epoch bump at truncate_env_frames_to_checkpoint）。
- 3 new observability counters (dual-path / compact-bytes / drift-prevented)
  pair with the existing #1446 / #1637 counters so dashboards observe
  the full lifecycle closed-loop observability surface.

## 优先级
**P0** (SoA 重构后一致性核心 gap，影响 correctness + 内存)

## 标签
P0, consistency, safety, SoA, EnvFrame, mutation, gc,
production-readiness, dual-epoch

## 相关 Issues
- #1014 (original MutationBoundaryGuard + dual-epoch foundation)
- #1446 (compact / re_pin_cow_children_from_snapshot pattern)
- #1478 (linear_post_mutate_enforce MVP)
- #1510 (compact_env_frames + bridge_epoch bump on truncate)
- #1542 (linear_post_mutate_enforce at materialize_call_env parity)
- #1660 (closure_is_epoch_or_env_stale dual-path stale contract)
- #1728 (commit_panic_checkpoint bumps bridge_epoch — same bug class)
- #1731 (linear_post_mutate_enforce(NULL_ENV_ID) observability)
- #1734 (bridge_epoch drift in GC root collection — slim_slim pattern)
- #1739 (bridge_epoch bump in truncate_env_frames_to_checkpoint —
  complementary to this issue, bumps the epoch at truncate so the
  walk_active_closures refresh step in #1637 is defensive double-check)
- #1637 (panic checkpoint lifecycle hardening — same-day ship,
  paired metric surface)
- #1362 (compact_mutation_log related open issue — long-running
  heavy-mutation leak observation)
- #1630 / #1632 (related epoch / provenance recent issues)

## 验证方式
- `tests/test_soa_dual_path_consistency.cpp`: 9 ACs all green
- `tests/test_issue_1478.cpp` extended: AC10/11/12 all green
- `scripts/check_soa_dual_path_consistency_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing `query:mutation-boundary-coverage-stats` surface extended
  within the 521 budget) + test-registry (#1572) + test binding +
  coverage (#1453)
- Same PR cycle as #1637 / #1908 / #1907: edit → build → run tests →
  descriptive commit → push `main` (direct, no PR review per MEMORY.md
  workflow).