# Issue #1639 — Per-block dirty bitmask → partial re-lower wiring

## 来源
EDSL 生产级 Code Review（2026-07-16）建议 #5（P0 性能关键）。
Building on #1474 (per-block dirty planning) + #1495 (Task1 followup)
+ #1505 (nested-lambda targeted dirty) + #1514 (#1625 per-block) +
#1555 (impact_scope body-only bitmask) + #1601 (consumer wiring) +
#1605 (eval_ir hot-path) + #1623 (per-block per-func). The
`IRCacheEntry::block_dirty_per_func_` + `mark_block_dirty` +
`cascade_block_to_instructions` infrastructure is already in place;
this issue wires it into the partial re-lower decision path with
explicit per-call observability counters.

## 问题描述
`IRCacheEntry` 已实现完整 `block_dirty_per_func_` / `instruction_dirty_per_func_`
位掩码 + `mark_block_dirty` / `cascade_block_to_instructions`，
`mark_define_dirty` 也会 cascade。但 **真正的 re-lower 路径
（lower_to_ir_with_cache_tracked / store_define_v2 后的消费者）
仍以全函数为粒度**，小范围 AI `mutate:set-body` / `mutate:rebind`
经常触发全量 re-lower，增量编译收益大幅退化。

具体 gap：
- `relower_define_blocks` 已消费 bitmask 做 passes (issue #1574)，但缺
  4 个 spec metrics (`full_relower_count` / `dirty_block_ratio` /
  `relower_block_hit_rate`) 的显式 observability。
- 现有 primitive `query:incremental-relower-stats` (schema 1623) 暴露了
  部分相关 counters (`full_relower_count` as alias + `dirty_block_ratio`
  as basis points from current snapshot)，但缺 running sums for
  time-weighted averages + `relower_block_hit_rate` 键。

## 代码证据 (code anchors)

### 修复前

```cpp
// service.ixx::relower_define_blocks — pre-#1639
metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);
// ... no explicit #1639 metric bumps ...

// evaluator_primitives_obs_eval_05.cpp — pre-#1639
//   schema = 1623, lineage 1605 / 1601 / 1506 / 718
//   exposes: impact-blocks-hit, partial-relowers, full-fallbacks,
//            time-saved-us, incremental_relower_blocks,
//            relower_per_function_called_count,
//            relower_skipped_entirely_count, relower_full_called_count,
//            full_relower_count (alias), dirty_block_ratio,
//            dirty_block_ratio_bp, incremental_eval_relower_hits,
//            eval_path_relower_total, eval_ir_path_relower_total
//   NOT exposed: running sums + relower_block_hit_rate
```

### 修复后 (Issue #1639 hardened)

```cpp
// service.ixx — full-fallback path (post-#1639)
metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);
evaluator_.bump_full_relower_count();
evaluator_.bump_relower_block_hit_rate(0, 1); // 0 incremental, 1 total attempt

// service.ixx — partial success path (post-#1639)
if (relower_define_function(name, dirty_func_idx, ...)) {
    // ... existing clean_funcs bump ...
    evaluator_.bump_relower_block_hit_rate(1, 1); // 1 hit, 1 total attempt
}

// service.ixx — entry lookup path (post-#1639)
evaluator_.bump_dirty_block_ratio(dirty_blocks, total_blocks_seen);

// evaluator_primitives_obs_eval_05.cpp — primitive output (post-#1639)
//   schema = 1639, lineage 718 → 1605 → 1601 → 1506 → 1623 → 1639
//   adds 6 new keys: full-relower-count, dirty-block-ratio-numerator-total,
//     dirty-block-ratio-denominator-total,
//     relower-block-hit-rate-numerator-total,
//     relower-block-hit-rate-denominator-total, relower-block-hit-rate
```

## 精确改动位置 (file-by-file)

1. **src/compiler/observability_metrics.h** — add 5 atomic counter slots
   (`full_relower_count` + 2 ratio pair + 2 hit-rate pair).

2. **src/compiler/compiler_metrics_fields.inc** — add 5 X-macro fields
   matching the new counters.

3. **src/compiler/evaluator.ixx** — add 5 bump_/getter pairs
   (`bump_full_relower_count` + `bump_dirty_block_ratio(num, den)` +
   `bump_relower_block_hit_rate(num, den)` + 5 get_ counterparts).

4. **src/compiler/service.ixx** — wire 4 bump sites in
   `relower_define_blocks`:
     - full-fallback path: `bump_full_relower_count` + `bump_relower_block_hit_rate(0, 1)`
     - partial success path (after `relower_define_function true`):
       `bump_relower_block_hit_rate(1, 1)`
     - entry lookup path: `bump_dirty_block_ratio(dirty_blocks, total_blocks_seen)`

5. **src/compiler/evaluator_primitives_obs_eval_05.cpp** — extend
   `query:incremental-relower-stats` kv list with 6 new keys +
   schema bump 1623 → 1639.

6. **tests/test_incremental_relower_perblock.cpp** (new, ~280 lines,
   7 ACs source-driven). Pattern: tests/test_issue_1638.cpp +
   tests/test_incremental_relower.cpp + tests/test_gc_roots_bridge_epoch_drift_1734.cpp.

7. **scripts/check_incremental_relower_coverage.py** (new, ~150 lines,
   10 ACs linter).

8. **CMakeLists.txt** — add `aura_add_issue_test(test_incremental_relower_perblock)`
   + `aura_issue_test_link_llvm_jit(test_incremental_relower_perblock)`
   entries.

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_incremental_relower_perblock` AC1: per-block dirty bitmask wired into 5 local passes (ConstantFolding / ComputeKind / TypePropagation / Shape / EscapeAnalysis) via `run_incremental_dirty_pipeline` |
| AC2 | AC2: 5 metric slots in `observability_metrics.h` |
| AC3 | AC3: 5 `bump_/get_` pairs declared in `evaluator.ixx` |
| AC4 | AC4: 4 wire-up sites in `relower_define_blocks` (full_bump + partial_bump + full_hit_rate + ratio_bump) |
| AC5 | AC5: `query:incremental-relower-stats` extended with 6 new keys + schema 1639 |
| AC6 | AC6: 5 X-macro fields in `compiler_metrics_fields.inc` |
| AC7 | AC7: cross-layer baseline regression — `CompilerService` round-trip still works |

10 linter ACs in `scripts/check_incremental_relower_coverage.py`:
production-code wire-up checks + metric slot presence + bump_/getter pair
presence + 4 wire-up sites + primitive extension + schema bump.

## 预期收益
- AI 多轮自修改热路径真正细粒度增量，典型小编辑 re-lower blocks 数量
  大幅减少（existing infrastructure already does per-block passes via
  mask_ptr; this issue completes the observability surface for the
  partial-vs-full decision）。
- 支撑 EDSL 生产级闭环编辑性能（latency 可预测 — dashboards can
  observe hit-rate + dirty-ratio in real time）。
- 向 #1474 / #1495 / #1605 目标推进 — adds the 4 spec metrics the
  prior issues couldn't expose because the dual-write dual-aggregate
  pattern wasn't finalized.

## 优先级
**P0**（直接决定 AI 自修改热路径是否可用）

## 标签
P0, performance, incremental, mutation, production-readiness, ai-agent

## 相关 Issues
- #1474（原 per-block dirty 规划）
- #1495（Task1 followup）
- #1505（nested-lambda targeted dirty + dep_graph 钩子）
- #1514（per-block nested lambda targeting）
- #1555（impact_scope body-only bitmask）
- #1601（consumer wiring）
- #1605（eval_ir hot-path partial re-lower）
- #1623（per-block per-func observability schema 1623 — the immediate
  parent of #1639）

## 验证方式
- `tests/test_incremental_relower_perblock.cpp`: 7 ACs all green
- `scripts/check_incremental_relower_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing `query:incremental-relower-stats` surface extended within
  the 521 budget) + test-registry (#1572) + test binding + coverage (#1453)
- Same PR cycle as #1908 / #1907 / #1637 / #1638: edit → build → run
  tests → descriptive commit → push `main` (direct, no PR review per
  MEMORY.md workflow).